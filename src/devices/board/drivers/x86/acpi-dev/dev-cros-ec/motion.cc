// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This driver uses zxlogf level DEBUG1 for logging all report processing actions.
// This is an especially verbose datastream.
//
// Future work for this driver:
// - Move individual sensor configuration to be Feature Report based.  The
//   standard specifies ways of talking about sampling rates.
// - Support requesting reports directly from the hardware with the HidbusGetReport
//   interface.
// - Synchronize the sensor FIFO better; the hardware provides support for
//   dropping a marker into the FIFO so you can synchronize (c.f. the FLUSH
//   subcommand of the MOTIONSENSE command).

#include "motion.h"

#include <lib/ddk/debug.h>
#include <stdio.h>
#include <stdlib.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <memory>
#include <utility>

#include <acpica/acpi.h>
#include <chromiumos-platform-ec/ec_commands.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/array.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <fbl/ref_ptr.h>
#include <fbl/span.h>
#include <hid/descriptor.h>

#include "../../include/errors.h"
#include "acpi.h"
#include "dev.h"

namespace cros_ec {

namespace {
// Convert a sensor index into a HID report ID.
//
// Report ID is 0, so we need to offset all sensors by 1.
uint8_t SensorIdToReportId(uint8_t sensor_id) {
  ZX_DEBUG_ASSERT(sensor_id < UINT8_MAX);
  return sensor_id + 1;
}
}  // namespace

AcpiCrOsEcMotionDevice::AcpiCrOsEcMotionDevice(fbl::RefPtr<EmbeddedController> ec,
                                               zx_device_t* parent,
                                               std::unique_ptr<AcpiHandle> acpi_handle)
    : DeviceType(parent), ec_(std::move(ec)), acpi_handle_(std::move(acpi_handle)) {}

AcpiCrOsEcMotionDevice::~AcpiCrOsEcMotionDevice() {}

void AcpiCrOsEcMotionDevice::NotifyHandler(ACPI_HANDLE handle, UINT32 value, void* ctx) {
  auto dev = reinterpret_cast<AcpiCrOsEcMotionDevice*>(ctx);

  zxlogf(TRACE, "acpi-cros-ec-motion: got event 0x%x", value);
  switch (value) {
    case 0x80:
      fbl::AutoLock guard(&dev->hid_lock_);
      dev->ConsumeFifoLocked();
      break;
  }
}

zx_status_t AcpiCrOsEcMotionDevice::ConsumeFifoLocked() {
  struct ec_response_motion_sensor_data data;
  zx_status_t status;
  while ((status = FifoRead(&data)) == ZX_OK) {
    if (data.sensor_num >= sensors_.size() || !sensors_[data.sensor_num].valid) {
      continue;
    }
    if (data.flags & (MOTIONSENSE_SENSOR_FLAG_TIMESTAMP | MOTIONSENSE_SENSOR_FLAG_FLUSH)) {
      // This is a special packet, not a report.
      continue;
    }

    uint8_t report[8] = {SensorIdToReportId(data.sensor_num)};
    size_t report_len = 1;
    switch (sensors_[data.sensor_num].type) {
      // 3-axis sensors
      case MOTIONSENSE_TYPE_ACCEL:
      case MOTIONSENSE_TYPE_GYRO:
      case MOTIONSENSE_TYPE_MAG:
        static_assert(sizeof(data.data) == 6, "");
        memcpy(report + report_len, data.data, sizeof(data.data));
        report_len += sizeof(data.data);
        break;
      // 1-axis sensors
      case MOTIONSENSE_TYPE_LIGHT:
        static_assert(sizeof(data.data[0]) == 2, "");
        memcpy(report + report_len, data.data, sizeof(data.data[0]));
        report_len += 2;
        break;
      default:
        ZX_ASSERT_MSG(false, "should not be reachable\n");
    }

    ZX_DEBUG_ASSERT(report_len < sizeof(report));
    QueueHidReportLocked(report, report_len);
  }
  return (status == ZX_ERR_SHOULD_WAIT) ? ZX_OK : status;
}

void AcpiCrOsEcMotionDevice::QueueHidReportLocked(const uint8_t* data, size_t len) {
  // Default unit is lux
  if (client_.is_valid()) {
    client_.IoQueue(data, len, zx_clock_get_monotonic());
  }
}

zx_status_t AcpiCrOsEcMotionDevice::HidbusQuery(uint32_t options, hid_info_t* info) {
  zxlogf(DEBUG, "acpi-cros-ec-motion: hid bus query");

  info->dev_num = 0;
  info->device_class = HID_DEVICE_CLASS_OTHER;
  info->boot_device = false;
  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::HidbusStart(const hidbus_ifc_protocol_t* ifc) {
  zxlogf(DEBUG, "acpi-cros-ec-motion: hid bus start");

  fbl::AutoLock guard(&hid_lock_);
  if (client_.is_valid()) {
    return ZX_ERR_ALREADY_BOUND;
  }

  client_ = ddk::HidbusIfcProtocolClient(ifc);

  zx_status_t status = FifoInterruptEnable(true);
  if (status != ZX_OK) {
    return status;
  }

  // TODO(teisenbe): Make this setting dynamic
  // Enable all of our sensors at 10000mHz
  const uint8_t num_sensors = static_cast<uint8_t>(sensors_.size());
  for (uint8_t i = 0; i < num_sensors; ++i) {
    if (!sensors_[i].valid) {
      continue;
    }

    status = SetSensorOutputDataRate(i, 10000);
    if (status != ZX_OK) {
      zxlogf(ERROR, "acpi-cros-ec-motion: set sensor %u odr failed: %d", i, status);
      continue;
    }
    status = SetEcSamplingRate(i, 100);
    if (status != ZX_OK) {
      zxlogf(ERROR, "acpi-cros-ec-motion: set sensor %u ec sample rate failed: %d", i, status);
      continue;
    }
  }

  status = ConsumeFifoLocked();
  if (status != ZX_OK) {
    FifoInterruptEnable(false);
    return status;
  }

  return ZX_OK;
}

void AcpiCrOsEcMotionDevice::HidbusStop() {
  zxlogf(DEBUG, "acpi-cros-ec-motion: hid bus stop");

  fbl::AutoLock guard(&hid_lock_);

  client_.clear();
  FifoInterruptEnable(false);

  // Disable all sensors
  const uint8_t num_sensors = static_cast<uint8_t>(sensors_.size());
  for (uint8_t i = 0; i < num_sensors; ++i) {
    if (!sensors_[i].valid) {
      continue;
    }

    zx_status_t status = SetSensorOutputDataRate(i, 0);
    if (status != ZX_OK) {
      zxlogf(ERROR, "acpi-cros-ec-motion: set sensor %u odr failed: %d", i, status);
      continue;
    }

    status = SetEcSamplingRate(i, 0);
    if (status != ZX_OK) {
      zxlogf(ERROR, "acpi-cros-ec-motion: set sensor %u ec sample rate failed: %d", i, status);
      continue;
    }
  }
}

zx_status_t AcpiCrOsEcMotionDevice::HidbusGetDescriptor(hid_description_type_t desc_type,
                                                        uint8_t* out_data_buffer, size_t data_size,
                                                        size_t* out_data_actual) {
  zxlogf(DEBUG, "acpi-cros-ec-motion: hid bus get descriptor");

  if (out_data_buffer == nullptr || out_data_actual == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (desc_type != HID_DESCRIPTION_TYPE_REPORT) {
    return ZX_ERR_NOT_FOUND;
  }

  if (data_size < hid_descriptor_.size()) {
    return ZX_ERR_BUFFER_TOO_SMALL;
  }

  memcpy(out_data_buffer, hid_descriptor_.get(), hid_descriptor_.size());
  *out_data_actual = hid_descriptor_.size();
  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::HidbusGetReport(uint8_t rpt_type, uint8_t rpt_id, uint8_t* data,
                                                    size_t len, size_t* out_len) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t AcpiCrOsEcMotionDevice::HidbusSetReport(uint8_t rpt_type, uint8_t rpt_id,
                                                    const uint8_t* data, size_t len) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t AcpiCrOsEcMotionDevice::HidbusGetIdle(uint8_t rpt_id, uint8_t* duration) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t AcpiCrOsEcMotionDevice::HidbusSetIdle(uint8_t rpt_id, uint8_t duration) {
  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::HidbusGetProtocol(uint8_t* protocol) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t AcpiCrOsEcMotionDevice::HidbusSetProtocol(uint8_t protocol) { return ZX_OK; }

void AcpiCrOsEcMotionDevice::DdkRelease() {
  zxlogf(INFO, "acpi-cros-ec-motion: release");
  delete this;
}

zx_status_t AcpiCrOsEcMotionDevice::QueryNumSensors(uint8_t* count) {
  zxlogf(DEBUG, "acpi-cros-ec-motion: QueryNumSensors");
  struct ec_params_motion_sense cmd = {};
  cmd.cmd = MOTIONSENSE_CMD_DUMP;
  cmd.dump.max_sensor_count = 0;  // We only care about the number of sensors.

  // Get information about the sensors.
  struct ec_response_motion_sense rsp;
  zx_status_t status = ec_->IssueCommand(EC_CMD_MOTION_SENSE_CMD, 3, cmd, rsp.dump);
  if (status != ZX_OK) {
    return status;
  }

  *count = rsp.dump.sensor_count;
  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::QuerySensorInfo(uint8_t sensor_num, SensorInfo* info) {
  zxlogf(DEBUG, "acpi-cros-ec-motion: QuerySensorInfo %d", sensor_num);

  struct ec_params_motion_sense cmd = {};
  cmd.cmd = MOTIONSENSE_CMD_INFO;
  cmd.info_3.sensor_num = sensor_num;

  struct ec_response_motion_sense rsp;
  zx_status_t status = ec_->IssueCommand(EC_CMD_MOTION_SENSE_CMD, 3, cmd, rsp.info_3);
  if (status != ZX_OK) {
    return status;
  }

  if (rsp.info_3.type >= MOTIONSENSE_TYPE_MAX || rsp.info_3.location >= MOTIONSENSE_LOC_MAX) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  info->type = static_cast<motionsensor_type>(rsp.info_3.type);
  info->loc = static_cast<motionsensor_location>(rsp.info_3.location);
  info->min_sampling_freq = rsp.info_3.min_frequency;
  info->max_sampling_freq = rsp.info_3.max_frequency;
  info->fifo_max_event_count = rsp.info_3.fifo_max_event_count;
  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::FifoInterruptEnable(bool enable) {
  zxlogf(DEBUG, "acpi-cros-ec-motion: FifoInterruptEnable %d", enable);

  struct ec_params_motion_sense cmd = {};
  struct ec_response_motion_sense rsp;
  cmd.cmd = MOTIONSENSE_CMD_FIFO_INT_ENABLE;
  cmd.fifo_int_enable.enable = enable;

  zx_status_t status = ec_->IssueCommand(EC_CMD_MOTION_SENSE_CMD, 3, cmd, rsp.fifo_int_enable);
  if (status != ZX_OK) {
    return status;
  }

  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::SetSensorOutputDataRate(uint8_t sensor_num,
                                                            uint32_t freq_millihertz) {
  zxlogf(DEBUG, "acpi-cros-ec-motion: SetSensorOutputDataRate %d %u", sensor_num, freq_millihertz);

  struct ec_params_motion_sense cmd = {};
  struct ec_response_motion_sense rsp;
  cmd.cmd = MOTIONSENSE_CMD_SENSOR_ODR;
  cmd.sensor_odr.sensor_num = sensor_num;
  cmd.sensor_odr.roundup = 0;
  cmd.sensor_odr.data = static_cast<int32_t>(freq_millihertz);

  zx_status_t status = ec_->IssueCommand(EC_CMD_MOTION_SENSE_CMD, 3, cmd, rsp.sensor_odr);
  if (status != ZX_OK) {
    return status;
  }

  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::SetEcSamplingRate(uint8_t sensor_num, uint32_t milliseconds) {
  zxlogf(DEBUG, "acpi-cros-ec-motion: SetEcSamplingRate %d %u", sensor_num, milliseconds);

  struct ec_params_motion_sense cmd = {};
  struct ec_response_motion_sense rsp;
  cmd.cmd = MOTIONSENSE_CMD_EC_RATE;
  cmd.ec_rate.sensor_num = sensor_num;
  cmd.ec_rate.roundup = 0;
  cmd.ec_rate.data = static_cast<int32_t>(milliseconds);

  zx_status_t status = ec_->IssueCommand(EC_CMD_MOTION_SENSE_CMD, 3, cmd, rsp.ec_rate);
  if (status != ZX_OK) {
    return status;
  }

  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::GetSensorRange(uint8_t sensor_num, int32_t* range) {
  zxlogf(DEBUG, "acpi-cros-ec-motion: GetSensorRange %d", sensor_num);

  struct ec_params_motion_sense cmd = {};
  struct ec_response_motion_sense rsp;
  cmd.cmd = MOTIONSENSE_CMD_SENSOR_RANGE;
  cmd.sensor_range.sensor_num = sensor_num;
  cmd.sensor_range.roundup = 0;
  cmd.sensor_range.data = EC_MOTION_SENSE_NO_VALUE;

  zx_status_t status = ec_->IssueCommand(EC_CMD_MOTION_SENSE_CMD, 3, cmd, rsp.sensor_range);
  if (status != ZX_OK) {
    return status;
  }

  *range = rsp.sensor_range.ret;
  zxlogf(TRACE, "acpi-cros-ec-motion: sensor range %d: %d", sensor_num, *range);
  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::GetKbWakeAngle(int32_t* angle) {
  zxlogf(DEBUG, "acpi-cros-ec-motion: GetKbWakeAngle");

  struct ec_params_motion_sense cmd = {};
  struct ec_response_motion_sense rsp;
  cmd.cmd = MOTIONSENSE_CMD_KB_WAKE_ANGLE;
  cmd.kb_wake_angle.data = EC_MOTION_SENSE_NO_VALUE;

  zx_status_t status = ec_->IssueCommand(EC_CMD_MOTION_SENSE_CMD, 3, cmd, rsp.kb_wake_angle);
  if (status != ZX_OK) {
    return status;
  }

  *angle = rsp.kb_wake_angle.ret;
  zxlogf(TRACE, "acpi-cros-ec-motion: kb_wake_angle %d", *angle);
  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::SetKbWakeAngle(int16_t angle) {
  zxlogf(DEBUG, "acpi-cros-ec-motion: SetKbWakeAngle %d", angle);

  if (angle < 0 || angle > 360) {
    return ZX_ERR_INVALID_ARGS;
  }

  struct ec_params_motion_sense cmd = {};
  struct ec_response_motion_sense rsp;
  cmd.cmd = MOTIONSENSE_CMD_KB_WAKE_ANGLE;
  cmd.kb_wake_angle.data = angle;

  zx_status_t status = ec_->IssueCommand(EC_CMD_MOTION_SENSE_CMD, 3, cmd, rsp.kb_wake_angle);
  if (status != ZX_OK) {
    return status;
  }

  zxlogf(TRACE, "acpi-cros-ec-motion: kb_wake_angle %d", rsp.kb_wake_angle.ret);
  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::FifoRead(struct ec_response_motion_sensor_data* data) {
  zxlogf(TRACE, "acpi-cros-ec-motion: FifoRead");

  struct ec_params_motion_sense cmd = {};
  struct __packed {
    uint32_t count;
    struct ec_response_motion_sensor_data data;
  } rsp;
  cmd.cmd = MOTIONSENSE_CMD_FIFO_READ;
  cmd.fifo_read.max_data_vector = 1;

  size_t actual;
  zx_status_t status =
      ec_->IssueCommand(EC_CMD_MOTION_SENSE_CMD, 3, &cmd, sizeof(cmd), &rsp, sizeof(rsp), &actual);
  if (status != ZX_OK) {
    return status;
  }
  if (actual < sizeof(uint32_t)) {
    return ZX_ERR_IO;
  }
  if (rsp.count != 1) {
    zxlogf(TRACE, "acpi-cros-ec-motion: FifoRead found no reports");
    return ZX_ERR_SHOULD_WAIT;
  }
  if (actual != sizeof(rsp)) {
    return ZX_ERR_IO;
  }

  zxlogf(TRACE, "acpi-cros-ec-motion: sensor=%u flags=%#x val=(%d, %d, %d)", rsp.data.sensor_num,
         rsp.data.flags, rsp.data.data[0], rsp.data.data[1], rsp.data.data[2]);
  memcpy(data, &rsp.data, sizeof(*data));
  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::Bind(zx_device_t* parent, fbl::RefPtr<EmbeddedController> ec,
                                         std::unique_ptr<AcpiHandle> acpi_handle,
                                         AcpiCrOsEcMotionDevice** device) {
  // Ensure Motion Sense is supported by the EC.
  if (!ec->SupportsFeature(EC_FEATURE_MOTION_SENSE) ||
      !ec->SupportsFeature(EC_FEATURE_MOTION_SENSE_FIFO)) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  // Create the device.
  fbl::AllocChecker ac;
  AcpiHandle* acpi_handle_ptr = acpi_handle.get();
  std::unique_ptr<AcpiCrOsEcMotionDevice> dev(
      new (&ac) AcpiCrOsEcMotionDevice(std::move(ec), parent, std::move(acpi_handle)));
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  // Find motion sensors.
  zx_status_t status = dev->ProbeSensors();
  if (status != ZX_OK) {
    return status;
  }

  // Populate hid_descriptor_ based on available sensors.
  status = BuildHidDescriptor(fbl::Span(dev->sensors_.begin(), dev->sensors_.end()),
                              &dev->hid_descriptor_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "acpi-cros-ec-motion: failed to construct hid desc: %s",
           zx_status_get_string(status));
    return status;
  }

  // Install ACPI event handler
  status = acpi_handle_ptr->InstallNotifyHandler(ACPI_DEVICE_NOTIFY, NotifyHandler, dev.get());
  if (status != ZX_OK) {
    zxlogf(ERROR, "acpi-cros-ec-motion: could not install notify handler: %s",
           zx_status_get_string(status));
    return status;
  }

  // Bind.
  status = dev->DdkAdd("acpi-cros-ec-motion");
  if (status != ZX_OK) {
    return status;
  }

  // Ownership has transferred to the DDK, so release our unique_ptr, but
  // let the caller have a pointer to it.
  if (device != nullptr) {
    *device = dev.get();
  }
  (void)dev.release();
  return ZX_OK;
}

zx_status_t AcpiCrOsEcMotionDevice::ProbeSensors() {
  uint8_t num_sensors;
  zx_status_t status = QueryNumSensors(&num_sensors);
  if (status != ZX_OK) {
    zxlogf(ERROR, "acpi-cros-ec-motion: num sensors query failed: %d", status);
    return status;
  }
  zxlogf(DEBUG, "acpi-cros-ec-motion: found %u sensors", num_sensors);

  fbl::AllocChecker ac;
  sensors_.reserve(num_sensors, &ac);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  for (uint8_t i = 0; i < num_sensors; ++i) {
    SensorInfo info;
    status = QuerySensorInfo(i, &info);
    if (status != ZX_OK) {
      zxlogf(ERROR, "acpi-cros-ec-motion: sensor info query %u failed: %d", i, status);
      info.valid = false;
      sensors_.push_back(info);
      continue;
    }

    // Check if sensor type is supported
    switch (info.type) {
      case MOTIONSENSE_TYPE_ACCEL:
      case MOTIONSENSE_TYPE_GYRO:
      case MOTIONSENSE_TYPE_MAG:
      case MOTIONSENSE_TYPE_LIGHT:
        break;
      default:
        info.valid = false;
        sensors_.push_back(info);
        continue;
    }

    int32_t range;
    status = GetSensorRange(i, &range);
    if (status != ZX_OK) {
      zxlogf(ERROR, "acpi-cros-ec-motion: sensor range query %u failed: %d", i, status);
      info.valid = false;
      sensors_.push_back(info);
      continue;
    }

    zxlogf(TRACE, "acpi-cros-ec-motion: sensor %d: type=%u loc=%u freq=[%u,%u] evt_count=%u", i,
           info.type, info.loc, info.min_sampling_freq, info.max_sampling_freq,
           info.fifo_max_event_count);

    if (info.type == MOTIONSENSE_TYPE_MAG) {
      range *= 625;  // There are 625 uG in 1/16 uT.
    }

    switch (info.type) {
      case MOTIONSENSE_TYPE_ACCEL:
      case MOTIONSENSE_TYPE_GYRO:
      case MOTIONSENSE_TYPE_MAG:
        info.phys_min = -range;
        break;
      default:
        info.phys_min = 0;
        break;
    }
    info.phys_max = range;

    info.valid = true;
    sensors_.push_back(info);
  }
  return ZX_OK;
}

namespace {

constexpr uint8_t kHidDescriptorGroupPrologue[] = {
    HID_USAGE_PAGE(0x20),  // Usage Page (Sensors)
    HID_USAGE(0x01),       // Usage (Sensor)
    HID_COLLECTION_APPLICATION,
};
constexpr uint8_t kHidDescriptorGroupEpilogue[] = {
    HID_END_COLLECTION,
};

// Start all fragments with the report ID and phys params so we can easily overwrite
// them.  Report ID will become the sensor number.
#define SENSOR_PREAMBLE HID_REPORT_ID(0), HID_PHYSICAL_MIN32(0), HID_PHYSICAL_MAX32(0)

// Patch a descriptor that begins with SENSOR_PREAMBLE
void PatchDescriptor(uint8_t* desc, size_t len, uint8_t report_id, int32_t phys_min,
                     int32_t phys_max) {
  ZX_DEBUG_ASSERT_MSG(report_id >= 1, "Report ID 0 is reserved by HID spec");
  const uint8_t data[] = {
      HID_REPORT_ID(report_id),
      HID_PHYSICAL_MIN32(phys_min),
      HID_PHYSICAL_MAX32(phys_max),
  };
  static_assert(sizeof(data) == 12, "");
  ZX_DEBUG_ASSERT(len >= sizeof(data));
  memcpy(desc, data, sizeof(data));
}

constexpr uint8_t kHidDescriptorAccelerometer[] = {
    SENSOR_PREAMBLE,

    HID_USAGE_PAGE(0x20),  // Usage Page (Sensors)
    HID_USAGE(0x73),       // Usage (Motion: Accelerometer 3D)

    // input reports (transmit)
    HID_COLLECTION_PHYSICAL,
    HID_USAGE_PAGE(0x20),  // Usage Page (Sensors)

    HID_LOGICAL_MIN16(-32768), HID_LOGICAL_MAX16(32767),
    // Stay with default unit of G.
    HID_REPORT_SIZE(16), HID_REPORT_COUNT(1),

    HID_USAGE16(0x0453),  // Usage (Acceleration Axis X)
    HID_INPUT(0x3),       // Const Var Abs
    HID_USAGE16(0x0454),  // Usage (Acceleration Axis Y)
    HID_INPUT(0x3),       // Const Var Abs
    HID_USAGE16(0x0455),  // Usage (Acceleration Axis Z)
    HID_INPUT(0x3),       // Const Var Abs
    HID_END_COLLECTION};

constexpr uint8_t kHidDescriptorGyroscope[] = {
    SENSOR_PREAMBLE,

    HID_USAGE_PAGE(0x20),  // Usage Page (Sensors)
    HID_USAGE(0x76),       // Usage (Motion: Gyrometer 3D)

    // input reports (transmit)
    HID_COLLECTION_PHYSICAL,
    HID_USAGE_PAGE(0x20),  // Usage Page (Sensors)

    HID_LOGICAL_MIN16(-32768), HID_LOGICAL_MAX16(32767),
    // Stay with default unit of deg/s.
    HID_REPORT_SIZE(16), HID_REPORT_COUNT(1),

    HID_USAGE16(0x0457),  // Usage (Angular Velocity about X Axis)
    HID_INPUT(0x3),       // Const Var Abs
    HID_USAGE16(0x0458),  // Usage (Angular Velocity about Y Axis)
    HID_INPUT(0x3),       // Const Var Abs
    HID_USAGE16(0x0459),  // Usage (Angular Velocity about Z Axis)
    HID_INPUT(0x3),       // Const Var Abs
    HID_END_COLLECTION};

constexpr uint8_t kHidDescriptorMagnetometer[] = {
    SENSOR_PREAMBLE,

    HID_USAGE_PAGE(0x20),  // Usage Page (Sensors)
    HID_USAGE(0x83),       // Usage (Motion: Compass 3D)

    // input reports (transmit)
    HID_COLLECTION_PHYSICAL,
    HID_USAGE_PAGE(0x20),  // Usage Page (Sensors)

    HID_LOGICAL_MIN16(-32768), HID_LOGICAL_MAX16(32767),
    // Scale so physical unit corresponds to 1/16 uT.  Default unit is
    // milligauss.  1/16 uT = 625 * 10^-3 mG.
    HID_UNIT_EXPONENT(-3), HID_REPORT_SIZE(16), HID_REPORT_COUNT(1),

    HID_USAGE16(0x0485),  // Usage (Magnetic Flux X Axis)
    HID_INPUT(0x3),       // Const Var Abs
    HID_USAGE16(0x0486),  // Usage (Magnetic Flux Y Axis)
    HID_INPUT(0x3),       // Const Var Abs
    HID_USAGE16(0x0487),  // Usage (Magnetic Flux Z Axis)
    HID_INPUT(0x3),       // Const Var Abs
    HID_END_COLLECTION};

constexpr uint8_t kHidDescriptorAmbientLight[] = {
    SENSOR_PREAMBLE,

    HID_USAGE_PAGE(0x20),  // Usage Page (Sensors)
    HID_USAGE(0x41),       // Usage (Light: Ambient Light)

    // input reports (transmit)
    HID_COLLECTION_PHYSICAL,
    HID_USAGE_PAGE(0x20),  // Usage Page (Sensors)

    HID_LOGICAL_MIN(0),
    HID_LOGICAL_MAX16(32767),  // TODO(teisenbe): Not sure if this value is right
    // Default unit is lux
    HID_REPORT_SIZE(16), HID_REPORT_COUNT(1),

    HID_USAGE16(0x04d1),  // Usage (Illuminance)
    HID_INPUT(0x3),       // Const Var Abs
    HID_END_COLLECTION};

#undef SENSOR_PREAMBLE

struct HidDescSensorBlock {
  // Template of a sensor descriptor
  const uint8_t* block;
  // Length in bytes of the template
  size_t len;
};

// The template needed for a descriptor for a specific sensor type,
// indexed by |motionsensor_type| values.
constexpr HidDescSensorBlock kHidDescSensorBlock[] = {
    {kHidDescriptorAccelerometer, sizeof(kHidDescriptorAccelerometer)},
    {kHidDescriptorGyroscope, sizeof(kHidDescriptorGyroscope)},
    {kHidDescriptorMagnetometer, sizeof(kHidDescriptorMagnetometer)},
    {nullptr, 0},
    {kHidDescriptorAmbientLight, sizeof(kHidDescriptorAmbientLight)},
    {nullptr, 0},
    {nullptr, 0},
};
static_assert(std::size(kHidDescSensorBlock) == MOTIONSENSE_TYPE_MAX, "");

}  // namespace

zx_status_t BuildHidDescriptor(fbl::Span<const SensorInfo> sensors, fbl::Array<uint8_t>* result) {
  // We build out a descriptor with one top-level Application Collection for
  // each sensor location, and within each of these collections we have one
  // Physical Collection per sensor.
  size_t total_size = 0;
  bool loc_group_present[MOTIONSENSE_LOC_MAX] = {};
  for (const SensorInfo& sensor : sensors) {
    if (!sensor.valid) {
      continue;
    }

    if (kHidDescSensorBlock[sensor.type].len > 0) {
      loc_group_present[sensor.loc] = true;
      total_size += kHidDescSensorBlock[sensor.type].len;
    }
  }

  for (size_t i = 0; i < std::size(loc_group_present); ++i) {
    if (loc_group_present[i]) {
      total_size += sizeof(kHidDescriptorGroupPrologue) + sizeof(kHidDescriptorGroupEpilogue);
    }
  }

  fbl::AllocChecker ac;
  std::unique_ptr<uint8_t[]> desc(new (&ac) uint8_t[total_size]);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  uint8_t* p = desc.get();
  size_t len = total_size;

  for (size_t loc = 0; loc < std::size(loc_group_present); ++loc) {
    if (!loc_group_present[loc]) {
      continue;
    }

    memcpy(p, kHidDescriptorGroupPrologue, sizeof(kHidDescriptorGroupPrologue));
    p += sizeof(kHidDescriptorGroupPrologue);
    len -= sizeof(kHidDescriptorGroupPrologue);

    for (uint8_t i = 0; i < sensors.size(); ++i) {
      const SensorInfo& sensor = sensors[i];
      if (!sensor.valid || sensor.loc != loc) {
        continue;
      }

      const HidDescSensorBlock& ref_block = kHidDescSensorBlock[sensor.type];

      if (ref_block.len > 0) {
        memcpy(p, ref_block.block, ref_block.len);
        PatchDescriptor(p, len, SensorIdToReportId(i), static_cast<int32_t>(sensor.phys_min),
                        static_cast<int32_t>(sensor.phys_max));

        p += ref_block.len;
        len -= ref_block.len;
      }
    }

    memcpy(p, kHidDescriptorGroupEpilogue, sizeof(kHidDescriptorGroupEpilogue));
    p += sizeof(kHidDescriptorGroupEpilogue);
    len -= sizeof(kHidDescriptorGroupEpilogue);
  }

  if (len != 0) {
    return ZX_ERR_INTERNAL;
  }

  *result = fbl::Array<uint8_t>(desc.release(), total_size);
  return ZX_OK;
}

}  // namespace cros_ec

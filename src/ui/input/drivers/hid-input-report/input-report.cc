// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input-report.h"

#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/platform-defs.h>
#include <lib/fidl/epitaph.h>
#include <lib/fit/defer.h>
#include <lib/zx/clock.h>
#include <threads.h>
#include <zircon/status.h>

#include <array>

#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <hid-parser/parser.h>
#include <hid-parser/report.h>
#include <hid-parser/usages.h>

#include "src/ui/input/drivers/hid-input-report/hid_input_report_bind.h"
#include "src/ui/input/lib/hid-input-report/device.h"

namespace hid_input_report_dev {

void InputReport::DdkUnbind(ddk::UnbindTxn txn) {
  hiddev_.UnregisterListener();
  txn.Reply();
}

void InputReport::RemoveReaderFromList(InputReportsReader* reader) {
  fbl::AutoLock lock(&readers_lock_);
  for (auto iter = readers_list_.begin(); iter != readers_list_.end(); ++iter) {
    if (iter->get() == reader) {
      readers_list_.erase(iter);
      break;
    }
  }
}

void InputReport::HidReportListenerReceiveReport(const uint8_t* report, size_t report_size,
                                                 zx_time_t report_time) {
  fbl::AutoLock lock(&readers_lock_);
  for (auto& device : devices_) {
    // Find the matching device.
    if (device->InputReportId() != 0 && device->InputReportId() != report[0]) {
      continue;
    }

    for (auto& reader : readers_list_) {
      reader->ReceiveReport(report, report_size, report_time, device.get());
    }
  }

  const zx::duration latency = zx::clock::get_monotonic() - zx::time(report_time);

  total_latency_ += latency;
  report_count_++;
  average_latency_usecs_.Set(total_latency_.to_usecs() / report_count_);

  if (latency > max_latency_) {
    max_latency_ = latency;
    max_latency_usecs_.Set(max_latency_.to_usecs());
  }

  latency_histogram_usecs_.Insert(latency.to_usecs());
}

bool InputReport::ParseHidInputReportDescriptor(const hid::ReportDescriptor* report_desc) {
  std::unique_ptr<hid_input_report::Device> device;
  hid_input_report::ParseResult result = hid_input_report::CreateDevice(report_desc, &device);
  if (result != hid_input_report::ParseResult::kOk) {
    return false;
  }
  devices_.push_back(std::move(device));
  return true;
}

void InputReport::SendInitialConsumerControlReport(InputReportsReader* reader) {
  for (auto& device : devices_) {
    if (device->GetDeviceType() == hid_input_report::DeviceType::kConsumerControl) {
      std::array<uint8_t, HID_MAX_REPORT_LEN> report_data;
      size_t report_size = 0;
      zx_status_t status = hiddev_.GetReport(HID_REPORT_TYPE_INPUT, device->InputReportId(),
                                             report_data.data(), report_data.size(), &report_size);
      if (status != ZX_OK) {
        continue;
      }
      reader->ReceiveReport(report_data.data(), report_size, zx_clock_get_monotonic(),
                            device.get());
    }
  }
}

std::string InputReport::GetDeviceTypesString() const {
  const char* kDeviceTypeNames[] = {
      [static_cast<uint32_t>(hid_input_report::DeviceType::kMouse)] = "mouse",
      [static_cast<uint32_t>(hid_input_report::DeviceType::kSensor)] = "sensor",
      [static_cast<uint32_t>(hid_input_report::DeviceType::kTouch)] = "touch",
      [static_cast<uint32_t>(hid_input_report::DeviceType::kKeyboard)] = "keyboard",
      [static_cast<uint32_t>(hid_input_report::DeviceType::kConsumerControl)] = "consumer-control",
  };

  std::string device_types;
  for (size_t i = 0; i < devices_.size(); i++) {
    if (i > 0) {
      device_types += ',';
    }

    const auto type = static_cast<uint32_t>(devices_[i]->GetDeviceType());
    if (type >= sizeof(kDeviceTypeNames) || !kDeviceTypeNames[type]) {
      device_types += "unknown";
    } else {
      device_types += kDeviceTypeNames[type];
    }
  }

  return device_types;
}

void InputReport::GetInputReportsReader(GetInputReportsReaderRequestView request,
                                        GetInputReportsReaderCompleter::Sync& completer) {
  fbl::AutoLock lock(&readers_lock_);

  auto reader = InputReportsReader::Create(this, next_reader_id_++, loop_->dispatcher(),
                                           std::move(request->reader));
  if (!reader) {
    return;
  }

  SendInitialConsumerControlReport(reader.get());
  readers_list_.push_back(std::move(reader));

  // Signal to a test framework (if it exists) that we are connected to a reader.
  sync_completion_signal(&next_reader_wait_);
}

void InputReport::GetDescriptor(GetDescriptorRequestView request,
                                GetDescriptorCompleter::Sync& completer) {
  fidl::Arena<kFidlDescriptorBufferSize> descriptor_allocator;
  fuchsia_input_report::wire::DeviceDescriptor descriptor(descriptor_allocator);

  hid_device_info_t info;
  hiddev_.GetHidDeviceInfo(&info);

  fuchsia_input_report::wire::DeviceInfo fidl_info;
  fidl_info.vendor_id = info.vendor_id;
  fidl_info.product_id = info.product_id;
  fidl_info.version = info.version;
  descriptor.set_device_info(descriptor_allocator, std::move(fidl_info));

  for (auto& device : devices_) {
    device->CreateDescriptor(descriptor_allocator, descriptor);
  }

  fidl::Result result = completer.Reply(std::move(descriptor));
  if (result.status() != ZX_OK) {
    zxlogf(ERROR, "GetDescriptor: Failed to send descriptor: %s\n",
           result.FormatDescription().c_str());
  }
}

void InputReport::SendOutputReport(SendOutputReportRequestView request,
                                   SendOutputReportCompleter::Sync& completer) {
  uint8_t hid_report[HID_MAX_DESC_LEN];
  size_t size;
  hid_input_report::ParseResult result = hid_input_report::ParseResult::kNotImplemented;
  for (auto& device : devices_) {
    result = device->SetOutputReport(&request->report, hid_report, sizeof(hid_report), &size);
    if (result == hid_input_report::ParseResult::kOk) {
      break;
    }
    // Returning an error other than kParseNotImplemented means the device was supposed
    // to set the Output report but hit an error. When this happens we return the error.
    if (result != hid_input_report::ParseResult::kNotImplemented) {
      break;
    }
  }
  if (result != hid_input_report::ParseResult::kOk) {
    completer.ReplyError(ZX_ERR_INTERNAL);
    return;
  }

  zx_status_t status = hiddev_.SetReport(HID_REPORT_TYPE_OUTPUT, hid_report[0], hid_report, size);
  if (status != ZX_OK) {
    completer.ReplyError(status);
    return;
  }
  completer.ReplySuccess();
}

zx_status_t InputReport::Bind() {
  uint8_t report_desc[HID_MAX_DESC_LEN];
  size_t report_desc_size;
  zx_status_t status = hiddev_.GetDescriptor(report_desc, HID_MAX_DESC_LEN, &report_desc_size);
  if (status != ZX_OK) {
    return status;
  }

  hid::DeviceDescriptor* dev_desc = nullptr;
  auto parse_res = hid::ParseReportDescriptor(report_desc, report_desc_size, &dev_desc);
  if (parse_res != hid::ParseResult::kParseOk) {
    zxlogf(ERROR, "hid-parser: parsing report descriptor failed with error %d", int(parse_res));
    return ZX_ERR_INTERNAL;
  }
  auto free_desc = fit::defer([dev_desc]() { hid::FreeDeviceDescriptor(dev_desc); });

  auto count = dev_desc->rep_count;
  if (count == 0) {
    zxlogf(ERROR, "No report descriptors found ");
    return ZX_ERR_INTERNAL;
  }

  // Parse each input report.
  for (size_t rep = 0; rep < count; rep++) {
    const hid::ReportDescriptor* desc = &dev_desc->report[rep];
    if (desc->input_count != 0) {
      if (!ParseHidInputReportDescriptor(desc)) {
        continue;
      }
    }
  }

  // If we never parsed a single device correctly then fail.
  if (devices_.size() == 0) {
    zxlogf(ERROR, "Can't process HID report descriptor for, all parsing attempts failed.");
    return ZX_ERR_INTERNAL;
  }

  // Start the async loop for the Readers.
  {
    fbl::AutoLock lock(&readers_lock_);
    loop_.emplace(&kAsyncLoopConfigNoAttachToCurrentThread);
    status = loop_->StartThread("hid-input-report-reader-loop");
    if (status != ZX_OK) {
      return status;
    }
  }

  // Register to listen to HID reports.
  status = hiddev_.RegisterListener(this, &hid_report_listener_protocol_ops_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to register for HID reports: %s", zx_status_get_string(status));
    return status;
  }

  const std::string device_types = GetDeviceTypesString();

  root_ = inspector_.GetRoot().CreateChild("hid-input-report-" + device_types);
  latency_histogram_usecs_ = root_.CreateExponentialUintHistogram(
      "latency_histogram_usecs", kLatencyFloor.to_usecs(), kLatencyInitialStep.to_usecs(),
      kLatencyStepMultiplier, kLatencyBucketCount);
  average_latency_usecs_ = root_.CreateUint("average_latency_usecs", 0);
  max_latency_usecs_ = root_.CreateUint("max_latency_usecs", 0);
  device_types_ = root_.CreateString("device_types", device_types);

  status = DdkAdd(ddk::DeviceAddArgs("InputReport").set_inspect_vmo(inspector_.DuplicateVmo()));
  if (status != ZX_OK) {
    hiddev_.UnregisterListener();
    return status;
  }

  return ZX_OK;
}

zx_status_t InputReport::WaitForNextReader(zx::duration timeout) {
  zx_status_t status = sync_completion_wait(&next_reader_wait_, timeout.get());
  if (status == ZX_OK) {
    sync_completion_reset(&next_reader_wait_);
  }
  return status;
}

zx_status_t input_report_bind(void* ctx, zx_device_t* parent) {
  fbl::AllocChecker ac;

  ddk::HidDeviceProtocolClient hiddev(parent);
  if (!hiddev.is_valid()) {
    return ZX_ERR_INTERNAL;
  }

  auto dev = fbl::make_unique_checked<InputReport>(&ac, parent, hiddev);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  auto status = dev->Bind();
  if (status == ZX_OK) {
    // devmgr is now in charge of the memory for dev
    __UNUSED auto ptr = dev.release();
  }
  return status;
}

static zx_driver_ops_t input_report_driver_ops = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = input_report_bind;
  return ops;
}();

}  // namespace hid_input_report_dev

ZIRCON_DRIVER(hid_input_report, hid_input_report_dev::input_report_driver_ops, "zircon", "0.1");

// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.hardware.input/cpp/wire.h>
#include <fidl/fuchsia.hardware.power.statecontrol/cpp/wire.h>
#include <lib/ddk/device.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/watcher.h>
#include <lib/fit/defer.h>
#include <lib/zx/channel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zircon/processargs.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#include <utility>

#include <fbl/alloc_checker.h>
#include <fbl/array.h>
#include <fbl/string.h>
#include <fbl/string_printf.h>
#include <fbl/unique_fd.h>
#include <hid-parser/parser.h>
#include <hid-parser/usages.h>

#include "src/sys/lib/stdout-to-debuglog/cpp/stdout-to-debuglog.h"

#define INPUT_PATH "/input"

namespace statecontrol_fidl = fuchsia_hardware_power_statecontrol;

namespace {

bool usage_eq(const hid::Usage& u1, const hid::Usage& u2) {
  return u1.page == u2.page && u1.usage == u2.usage;
}

// Search the report descriptor for a System Power Down input field within a
// Generic Desktop:System Control collection.
//
// This method assumes the HID descriptor does not contain more than one such field.
zx_status_t FindSystemPowerDown(const hid::DeviceDescriptor* desc, uint8_t* report_id,
                                size_t* bit_offset) {
  const hid::Usage system_control = {
      .page = static_cast<uint16_t>(hid::usage::Page::kGenericDesktop),
      .usage = static_cast<uint32_t>(hid::usage::GenericDesktop::kSystemControl),
  };

  const hid::Usage power_down = {
      .page = static_cast<uint16_t>(hid::usage::Page::kGenericDesktop),
      .usage = static_cast<uint32_t>(hid::usage::GenericDesktop::kSystemPowerDown),
  };

  // Search for the field
  for (size_t rpt_idx = 0; rpt_idx < desc->rep_count; ++rpt_idx) {
    const hid::ReportDescriptor& report = desc->report[rpt_idx];

    for (size_t i = 0; i < report.input_count; ++i) {
      const hid::ReportField& field = report.input_fields[i];

      if (!usage_eq(field.attr.usage, power_down)) {
        continue;
      }

      const hid::Collection* collection = hid::GetAppCollection(&field);
      if (!collection || !usage_eq(collection->usage, system_control)) {
        continue;
      }
      *report_id = field.report_id;
      *bit_offset = field.attr.offset;
      return ZX_OK;
    }
  }
  return ZX_ERR_NOT_FOUND;
}

struct PowerButtonInfo {
  std::optional<fidl::WireSyncClient<fuchsia_hardware_input::Device>> client;
  uint8_t report_id;
  size_t bit_offset;
  bool has_report_id_byte;
};

static zx_status_t InputDeviceAdded(int dirfd, int event, const char* name, void* cookie) {
  if (event != WATCH_EVENT_ADD_FILE) {
    return ZX_OK;
  }

  // Open the fd and get a FIDL client.
  // Note: the rust vfs used in an integration test for this does not support
  // using the DESCRIBE flag with a service node, which all of the functions
  // that use file descriptors send as they are trying to emulate posix
  // semantics. To work around this, clone the fd into a new handle and then use
  // fdio_service_connect_at, which does not use the DESCRIBE flag.
  zx::channel dir_chan;
  zx_status_t status = fdio_fd_clone(dirfd, dir_chan.reset_and_get_address());
  if (status != ZX_OK) {
    printf("pwrbtn-monitor: clone failed: %s\n", zx_status_get_string(status));
    return ZX_OK;
  }

  zx::channel chan, remote_chan;
  status = zx::channel::create(0, &chan, &remote_chan);
  if (status != ZX_OK) {
    printf("pwrbtn-monitor: failed to create channel: %d\n", status);
    return status;
  }
  status = fdio_service_connect_at(dir_chan.get(), name, remote_chan.release());
  if (status != ZX_OK) {
    printf("pwrbtn-monitor: service handle conversion failed: %d\n", status);
    return status;
  }
  auto client = fidl::WireSyncClient<fuchsia_hardware_input::Device>(std::move(chan));

  // Get the report descriptor.
  auto result = client.GetReportDesc();
  if (result.status() != ZX_OK) {
    return ZX_OK;
  }

  hid::DeviceDescriptor* desc;
  if (hid::ParseReportDescriptor(result->desc.data(), result->desc.count(), &desc) !=
      hid::kParseOk) {
    return ZX_OK;
  }
  auto cleanup_desc = fit::defer([desc]() { hid::FreeDeviceDescriptor(desc); });

  uint8_t report_id;
  size_t bit_offset;
  status = FindSystemPowerDown(desc, &report_id, &bit_offset);
  if (status != ZX_OK) {
    return ZX_OK;
  }

  auto info = reinterpret_cast<PowerButtonInfo*>(cookie);
  info->client = std::move(client);
  info->report_id = report_id;
  info->bit_offset = bit_offset;
  info->has_report_id_byte = (desc->rep_count > 1 || desc->report[0].report_id != 0);
  return ZX_ERR_STOP;
}

zx_status_t send_poweroff() {
  zx::channel channel_local, channel_remote;
  zx_status_t status = zx::channel::create(0, &channel_local, &channel_remote);
  if (status != ZX_OK) {
    printf("pwrbtn-monitor: failed to create channel: %d\n", status);
    return ZX_ERR_INTERNAL;
  }

  auto service =
      fbl::StringPrintf("/svc/%s", fidl::DiscoverableProtocolName<statecontrol_fidl::Admin>);
  status = fdio_service_connect(service.c_str(), channel_remote.release());
  if (status != ZX_OK) {
    printf("pwrbtn-monitor: failed to connect to service %s: %d\n", service.c_str(), status);
    return ZX_ERR_INTERNAL;
  }

  auto admin_client = fidl::WireSyncClient<statecontrol_fidl::Admin>(std::move(channel_local));
  auto resp = admin_client.Poweroff();

  if (!resp.ok()) {
    printf("pwrbtn-monitor: Call to %s failed: %s\n", service.c_str(),
           resp.FormatDescription().c_str());
    return resp.status();
  }

  return ZX_OK;
}

}  // namespace

int main(int argc, char** argv) {
  zx_status_t status = StdoutToDebuglog::Init();
  if (status != ZX_OK) {
    return 1;
  }
  fbl::unique_fd dirfd;
  {
    int fd = open(INPUT_PATH, O_DIRECTORY);
    if (fd < 0) {
      printf("pwrbtn-monitor: Failed to open " INPUT_PATH ": %d\n", errno);
      return 1;
    }
    dirfd.reset(fd);
  }

  PowerButtonInfo info;
  status = fdio_watch_directory(dirfd.get(), InputDeviceAdded, ZX_TIME_INFINITE, &info);
  if (status != ZX_ERR_STOP) {
    printf("pwrbtn-monitor: Failed to find power button device\n");
    return 1;
  }
  dirfd.reset();

  auto& client = *info.client;

  // Get the report event.
  //
  // // Get the report event.
  zx::event report_event;
  {
    auto result = client.GetReportsEvent();
    if (result.status() != ZX_OK) {
      printf("pwrbtn-monitor: failed to get report event: %d\n", result.status());
      return 1;
    }
    if (result->status != ZX_OK) {
      printf("pwrbtn-monitor: failed to get report event: %d\n", result->status);
      return 1;
    }
    report_event = std::move(result->event);
  }

  // Watch the power button device for reports
  while (true) {
    report_event.wait_one(ZX_USER_SIGNAL_0, zx::time::infinite(), nullptr);

    auto result = client.ReadReport();
    if (result.status() != ZX_OK) {
      printf("pwrbtn-monitor: failed to read report: %d\n", result.status());
      return 1;
    }
    if (result->status != ZX_OK) {
      printf("pwrbtn-monitor: failed to read report: %d\n", result->status);
      return 1;
    }

    const fidl::VectorView<uint8_t>& report = result->data;

    // Ignore reports from different report IDs
    if (info.has_report_id_byte && report[0] != info.report_id) {
      printf("pwrbtn-monitor: input-watcher: wrong id\n");
      continue;
    }

    // Check if the power button is pressed, and request a poweroff if so.
    const size_t byte_index = info.has_report_id_byte + info.bit_offset / 8;
    if (report[byte_index] & (1u << (info.bit_offset % 8))) {
      auto status = send_poweroff();
      if (status != ZX_OK) {
        printf("pwrbtn-monitor: input-watcher: failed send poweroff to device manager.\n");
        continue;
      }
    }
  }
}

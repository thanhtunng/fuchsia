// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/limits.h>
#include <lib/fdio/vfs.h>
#include <lib/zx/channel.h>
#include <unistd.h>
#include <zircon/compiler.h>
#include <zircon/device/vfs.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <iterator>
#include <new>
#include <vector>

#include <fbl/algorithm.h>
#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>
#include <fbl/vector.h>
#include <fs-management/mount.h>

#include "path.h"

namespace {

zx_status_t MkfsNativeFs(const char* binary, const char* device_path, LaunchCallback cb,
                         const MkfsOptions& options, bool support_fvm) {
  zx::channel crypt_client(options.crypt_client);
  fbl::unique_fd device_fd;
  device_fd.reset(open(device_path, O_RDWR));
  if (!device_fd) {
    fprintf(stderr, "Failed to open device\n");
    return ZX_ERR_BAD_STATE;
  }
  zx::channel block_device;
  zx_status_t status =
      fdio_get_service_handle(device_fd.release(), block_device.reset_and_get_address());
  if (status != ZX_OK) {
    return status;
  }
  fbl::Vector<const char*> argv;
  argv.push_back(binary);
  if (options.verbose) {
    argv.push_back("-v");
  }

  fbl::StringBuffer<20> fvm_data_slices;
  // TODO(manalib) restructure this code to do something more sensible instead of support_fvm bool.
  if (support_fvm) {
    MkfsOptions default_options;  // Use to get the default value.
    if (options.fvm_data_slices > default_options.fvm_data_slices) {
      argv.push_back("--fvm_data_slices");
      fvm_data_slices.AppendPrintf("%u", options.fvm_data_slices);
      argv.push_back(fvm_data_slices.c_str());
    }
  }

  if (options.deprecated_padded_blobfs_format) {
    argv.push_back("--deprecated_padded_format");
  }

  std::string inodes_str;
  if (options.num_inodes > 0) {
    argv.push_back("--num_inodes");
    inodes_str = std::to_string(options.num_inodes);
    argv.push_back(inodes_str.c_str());
  }

  argv.push_back("mkfs");
  argv.push_back(nullptr);

  zx_handle_t handles[] = {block_device.release(), crypt_client.release()};
  uint32_t ids[] = {FS_HANDLE_BLOCK_DEVICE_ID, PA_HND(PA_USER0, 2)};
  status = static_cast<zx_status_t>(cb(static_cast<int>(argv.size() - 1), argv.data(), handles, ids,
                                       handles[1] == ZX_HANDLE_INVALID ? 1 : 2));
  return status;
}

zx_status_t MkfsFat(const char* device_path, LaunchCallback cb, const MkfsOptions& options) {
  const std::string tool_path = fs_management::GetBinaryPath("mkfs-msdosfs");
  std::string sectors_per_cluster;
  std::vector<const char*> argv = {tool_path.c_str()};
  if (options.sectors_per_cluster != 0) {
    argv.push_back("-c");
    sectors_per_cluster = std::to_string(options.sectors_per_cluster);
    argv.push_back(sectors_per_cluster.c_str());
  }
  argv.push_back(device_path);
  argv.push_back(nullptr);
  return cb(argv.size() - 1, argv.data(), NULL, NULL, 0);
}

}  // namespace

__EXPORT
zx_status_t mkfs(const char* device_path, disk_format_t df, LaunchCallback cb,
                 const MkfsOptions& options) {
  // N.B. Make sure to release crypt_client in any new error paths here.
  switch (df) {
    case DISK_FORMAT_FACTORYFS:
      return MkfsNativeFs(fs_management::GetBinaryPath("factoryfs").c_str(), device_path, cb,
                          options, false);
    case DISK_FORMAT_MINFS:
      return MkfsNativeFs(fs_management::GetBinaryPath("minfs").c_str(), device_path, cb, options,
                          true);
    case DISK_FORMAT_FXFS:
      return MkfsNativeFs(fs_management::GetBinaryPath("fxfs").c_str(), device_path, cb, options,
                          true);
    case DISK_FORMAT_FAT:
      return MkfsFat(device_path, cb, options);
    case DISK_FORMAT_BLOBFS:
      return MkfsNativeFs(fs_management::GetBinaryPath("blobfs").c_str(), device_path, cb, options,
                          true);
    case DISK_FORMAT_F2FS:
      return MkfsNativeFs(fs_management::GetBinaryPath("f2fs").c_str(), device_path, cb, options,
                          true);
    default:
      auto* format = fs_management::CustomDiskFormat::Get(df);
      if (format == nullptr) {
        if (options.crypt_client != ZX_HANDLE_INVALID)
          zx_handle_close(options.crypt_client);
        return ZX_ERR_NOT_SUPPORTED;
      }
      return MkfsNativeFs(format->binary_path().c_str(), device_path, cb, options, true);
  }
}

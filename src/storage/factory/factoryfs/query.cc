// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/factory/factoryfs/query.h"

#include <lib/fidl-async/cpp/bind.h>

#include "src/storage/factory/factoryfs/factoryfs.h"
#include "src/storage/factory/factoryfs/format.h"
#include "src/storage/factory/factoryfs/runner.h"

namespace factoryfs {

namespace fuchsia_fs = fuchsia_fs;
using fuchsia_fs::wire::FilesystemInfoQuery;

constexpr char kFsName[] = "factoryfs";

QueryService::QueryService(async_dispatcher_t* dispatcher, Factoryfs* factoryfs, Runner* runner)
    : fs::Service([dispatcher, this](zx::channel server_end) {
        return fidl::BindSingleInFlightOnly(dispatcher, std::move(server_end), this);
      }),
      factoryfs_(factoryfs),
      runner_(runner) {}

void QueryService::GetInfo(GetInfoRequestView request, GetInfoCompleter::Sync& completer) {
  static_assert(sizeof(kFsName) < fuchsia_fs::wire::kMaxFsNameLength, "Factoryfs name too long");

  fidl::Arena allocator;
  fuchsia_fs::wire::FilesystemInfo filesystem_info(allocator);

  if (request->query & FilesystemInfoQuery::kTotalBytes) {
    // Account for 1 block for superblock.
    uint64_t num_blocks =
        1 + factoryfs_->Info().data_blocks + factoryfs_->Info().directory_ent_blocks;
    filesystem_info.set_total_bytes(allocator, num_blocks * factoryfs_->Info().block_size);
  }

  if (request->query & FilesystemInfoQuery::kUsedBytes) {
    filesystem_info.set_used_bytes(allocator,
                                   factoryfs_->Info().data_blocks * factoryfs_->Info().block_size);
  }

  if (request->query & FilesystemInfoQuery::kTotalNodes) {
    filesystem_info.set_total_nodes(allocator, factoryfs_->Info().directory_entries);
  }

  if (request->query & FilesystemInfoQuery::kUsedNodes) {
    filesystem_info.set_used_nodes(allocator, factoryfs_->Info().directory_entries);
  }

  if (request->query & FilesystemInfoQuery::kFsId) {
    filesystem_info.set_fs_id(allocator, factoryfs_->GetFsId());
  }

  if (request->query & FilesystemInfoQuery::kBlockSize) {
    filesystem_info.set_block_size(allocator, kFactoryfsBlockSize);
  }

  if (request->query & FilesystemInfoQuery::kMaxNodeNameSize) {
    filesystem_info.set_max_node_name_size(allocator, kFactoryfsMaxNameSize);
  }

  if (request->query & FilesystemInfoQuery::kFsType) {
    filesystem_info.set_fs_type(allocator, fuchsia_fs::wire::FsType::kFactoryfs);
  }

  if (request->query & FilesystemInfoQuery::kName) {
    fidl::StringView name(kFsName);
    filesystem_info.set_name(allocator, std::move(name));
  }

  char name_buf[fuchsia_io2::wire::kMaxPathLength];
  if (request->query & FilesystemInfoQuery::kDevicePath) {
    size_t name_len;
    zx_status_t status =
        factoryfs_->Device().GetDevicePath(fuchsia_io2::wire::kMaxPathLength, name_buf, &name_len);
    if (status != ZX_OK) {
      completer.ReplyError(status);
      return;
    }
    // It appears that the |name_len| returned by |GetDevicePath| includes a trailing NUL.
    ZX_ASSERT(name_buf[name_len - 1] == '\0');
    fidl::StringView device_path(name_buf, name_len - 1);
    filesystem_info.set_device_path(allocator, std::move(device_path));
  }

  completer.ReplySuccess(std::move(filesystem_info));
}

void QueryService::IsNodeInFilesystem(IsNodeInFilesystemRequestView request,
                                      IsNodeInFilesystemCompleter::Sync& completer) {
  completer.Reply(runner_->IsTokenAssociatedWithVnode(std::move(request->token)));
}

}  // namespace factoryfs

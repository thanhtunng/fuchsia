// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fdio.h"

#include <fidl/fuchsia.boot/cpp/wire.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/io.h>
#include <lib/fdio/spawn.h>
#include <lib/zircon-internal/paths.h>
#include <lib/zx/channel.h>
#include <lib/zx/debuglog.h>
#include <lib/zx/job.h>
#include <lib/zx/process.h>
#include <lib/zx/resource.h>
#include <lib/zx/vmo.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/processargs.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/log.h>

#include <utility>

#include <fbl/algorithm.h>
#include <fbl/array.h>
#include <fbl/string_buffer.h>
#include <fbl/vector.h>

#include "src/devices/lib/log/log.h"

#define CHILD_JOB_RIGHTS (ZX_RIGHTS_BASIC | ZX_RIGHT_MANAGE_JOB | ZX_RIGHT_MANAGE_PROCESS)

enum class FdioAction {
  AddNsEntry = 1,
  CloneDir = 2,
};

// clang-format off

const static struct {
    const char* mount;
    const char* name;
    uint32_t flags;
    FdioAction action;
} FSTAB[] = {
    { "/bin",       "bin",       FS_BIN,      FdioAction::CloneDir },
    { "/blob",      "blob",      FS_BLOB,     FdioAction::CloneDir },
    { "/boot",      "boot",      FS_BOOT,     FdioAction::CloneDir },
    { "/data",      "data",      FS_DATA,     FdioAction::CloneDir },
    { "/dev",       "dev",       FS_DEV,      FdioAction::AddNsEntry },
    // TODO(dgonyeo): add this path back in once the appmgr fragment doesn't
    // exist on bringup builds
    //{ "/hub",       "hub",       FS_HUB,      FdioAction::CloneDir },
    { "/install",   "install",   FS_INSTALL,  FdioAction::CloneDir },
    { "/durable",   "durable",   FS_DURABLE,  FdioAction::CloneDir },
    { "/factory",   "factory",   FS_FACTORY,  FdioAction::CloneDir },
    { "/pkgfs",     "pkgfs",     FS_PKGFS,    FdioAction::CloneDir },
    { "/svc",       "svc",       FS_SVC,      FdioAction::AddNsEntry },
    { "/system",    "system",    FS_SYSTEM,   FdioAction::CloneDir },
    { "/tmp",       "tmp",       FS_TMP,      FdioAction::CloneDir },
    { "/volume",    "volume",    FS_VOLUME,   FdioAction::CloneDir },
};

// clang-format on
//
FsProvider::~FsProvider() {}

DevmgrLauncher::DevmgrLauncher(FsProvider* fs_provider) : fs_provider_(fs_provider) {}

zx_status_t DevmgrLauncher::LaunchWithLoader(const zx::job& job, const char* name,
                                             zx::vmo executable, zx::channel loader,
                                             const char* const* argv, const char** initial_envp,
                                             int stdiofd, const zx::resource& root_resource,
                                             const zx_handle_t* handles, const uint32_t* types,
                                             size_t hcount, zx::process* out_proc, uint32_t flags) {
  zx::job job_copy;
  zx_status_t status = job.duplicate(CHILD_JOB_RIGHTS, &job_copy);
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to launch %s (%s), cannot duplicate job: %s", argv[0], name,
         zx_status_get_string(status));
    return status;
  }

  zx::debuglog debuglog;
  if (stdiofd < 0) {
    if ((status = zx::debuglog::create(root_resource, 0, &debuglog) != ZX_OK)) {
      return status;
    }
  }

  uint32_t spawn_flags = FDIO_SPAWN_CLONE_JOB | FDIO_SPAWN_CLONE_UTC_CLOCK;

  // Set up the environ for the new process
  fbl::Vector<const char*> env;
  if (getenv(LDSO_TRACE_CMDLINE)) {
    env.push_back(LDSO_TRACE_ENV);
  }
  env.push_back(ZX_SHELL_ENV_PATH);
  while (initial_envp && initial_envp[0]) {
    env.push_back(*initial_envp++);
  }
  env.push_back(nullptr);

  fbl::Vector<fdio_spawn_action_t> actions;
  actions.reserve(3 + std::size(FSTAB) + hcount);

  actions.push_back((fdio_spawn_action_t){
      .action = FDIO_SPAWN_ACTION_SET_NAME,
      .name = {.data = name},
  });

  if (loader.is_valid()) {
    actions.push_back((fdio_spawn_action_t){
        .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
        .h = {.id = PA_HND(PA_LDSVC_LOADER, 0), .handle = loader.release()},
    });
  } else {
    spawn_flags |= FDIO_SPAWN_DEFAULT_LDSVC;
  }

  // create namespace based on FS_* flags
  for (unsigned n = 0; n < std::size(FSTAB); n++) {
    if (!(FSTAB[n].flags & flags)) {
      continue;
    }
    switch (FSTAB[n].action) {
      case FdioAction::AddNsEntry: {
        zx_handle_t h;
        if ((h = fs_provider_->CloneFs(FSTAB[n].name).TakeChannel().release()) !=
            ZX_HANDLE_INVALID) {
          actions.push_back((fdio_spawn_action_t){
              .action = FDIO_SPAWN_ACTION_ADD_NS_ENTRY,
              .ns = {.prefix = FSTAB[n].mount, .handle = h},
          });
        }
      } break;
      case FdioAction::CloneDir:
        actions.push_back((fdio_spawn_action_t){
            .action = FDIO_SPAWN_ACTION_CLONE_DIR,
            .dir = {.prefix = FSTAB[n].mount},
        });
        break;
      default:
        __UNREACHABLE;
    }
  }

  auto action_uses_fd0 = [](const fdio_spawn_action_t& action) {
    switch (action.action) {
      case FDIO_SPAWN_ACTION_ADD_HANDLE:
        if (PA_HND_TYPE(action.h.id) == PA_FD) {
          int fd = PA_HND_ARG(action.h.id);
          return (fd & ~FDIO_FLAG_USE_FOR_STDIO) == 0;
        }
        break;
      case FDIO_SPAWN_ACTION_CLONE_FD:
      case FDIO_SPAWN_ACTION_TRANSFER_FD: {
        int fd = action.fd.target_fd;
        return (fd & ~FDIO_FLAG_USE_FOR_STDIO) == 0;
      }
    }
    return false;
  };

  if (std::none_of(actions.begin(), actions.end(), action_uses_fd0)) {
    if (debuglog.is_valid()) {
      actions.push_back((fdio_spawn_action_t){
          .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
          .h = {.id = PA_HND(PA_FD, FDIO_FLAG_USE_FOR_STDIO | 0), .handle = debuglog.release()},
      });
    } else {
      actions.push_back((fdio_spawn_action_t){
          .action = FDIO_SPAWN_ACTION_TRANSFER_FD,
          .fd = {.local_fd = stdiofd, .target_fd = FDIO_FLAG_USE_FOR_STDIO | 0},
      });
    }
  }

  for (size_t i = 0; i < hcount; ++i) {
    actions.push_back((fdio_spawn_action_t){
        .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
        .h = {.id = types[i], .handle = handles[i]},
    });
  }

  zx::process proc;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
  if (executable.is_valid()) {
    status = fdio_spawn_vmo(job_copy.get(), spawn_flags, executable.release(), argv, env.data(),
                            actions.size(), actions.data(), proc.reset_and_get_address(), err_msg);
  } else {
    status = fdio_spawn_etc(job_copy.get(), spawn_flags, argv[0], argv, env.data(), actions.size(),
                            actions.data(), proc.reset_and_get_address(), err_msg);
  }
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to launch %s (%s): %s", argv[0], name, err_msg);
    return status;
  }
  LOGF(INFO, "Launching %s (%s)", argv[0], name);
  if (out_proc != nullptr) {
    *out_proc = std::move(proc);
  }
  return ZX_OK;
}

zx_status_t DevmgrLauncher::Launch(const zx::job& job, const char* name, const char* const* argv,
                                   const char** initial_envp, int stdiofd,
                                   const zx::resource& root_resource, const zx_handle_t* handles,
                                   const uint32_t* types, size_t hcount, zx::process* out_proc,
                                   uint32_t flags) {
  return LaunchWithLoader(job, name, zx::vmo(), zx::channel(), argv, initial_envp, stdiofd,
                          root_resource, handles, types, hcount, out_proc, flags);
}

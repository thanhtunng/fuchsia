// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zircon/compiler.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <fs-management/mount.h>

namespace {
struct {
  const char* name;
  disk_format_t df;
} FILESYSTEMS[] = {
    {"blobfs", DISK_FORMAT_BLOBFS},       {"minfs", DISK_FORMAT_MINFS}, {"fat", DISK_FORMAT_FAT},
    {"factoryfs", DISK_FORMAT_FACTORYFS}, {"f2fs", DISK_FORMAT_F2FS},
};

int usage(void) {
  fprintf(stderr, "usage: fsck [ <option>* ] devicepath filesystem\n");
  fprintf(stderr, " -v  : Verbose mode\n");
  fprintf(stderr, " values for 'filesystem' include:\n");
  for (size_t i = 0; i < countof(FILESYSTEMS); i++) {
    fprintf(stderr, "  '%s'\n", FILESYSTEMS[i].name);
  }
  return -1;
}

int parse_args(int argc, char** argv, FsckOptions* options, disk_format_t* df, char** devicepath) {
  *df = DISK_FORMAT_UNKNOWN;
  while (argc > 1) {
    if (!strcmp(argv[1], "-v")) {
      options->verbose = true;
    } else {
      break;
    }
    argc--;
    argv++;
  }
  if (argc < 3) {
    return usage();
  }

  *devicepath = argv[1];
  for (size_t i = 0; i < countof(FILESYSTEMS); i++) {
    if (!strcmp(FILESYSTEMS[i].name, argv[2])) {
      *df = FILESYSTEMS[i].df;
      break;
    }
  }
  if (*df == DISK_FORMAT_UNKNOWN) {
    fprintf(stderr, "fs_fsck: Cannot check a device with filesystem '%s'\n", argv[2]);
    return -1;
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  char* devicepath;
  disk_format_t df;
  int r;
  FsckOptions options;
  if ((r = parse_args(argc, argv, &options, &df, &devicepath))) {
    return r;
  }

  if (options.verbose) {
    printf("fs_fsck: Checking device [%s]\n", devicepath);
  }

  if ((r = fsck(devicepath, df, options, launch_stdio_sync)) < 0) {
    fprintf(stderr, "fs_fsck: Failed to check device: %d\n", r);
  }
  return r;
}

// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_GUEST_VMM_VIRTIO_DEVICE_FAKE_H_
#define GARNET_BIN_GUEST_VMM_VIRTIO_DEVICE_FAKE_H_

#include "garnet/bin/guest/vmm/phys_mem_fake.h"
#include "garnet/bin/guest/vmm/virtio_device.h"
#include "garnet/bin/guest/vmm/virtio_queue_fake.h"

typedef struct test_config {
} test_config_t;

class VirtioDeviceFake
    : public VirtioInprocessDevice<UINT8_MAX, 1, test_config_t> {
 public:
  VirtioDeviceFake()
      : VirtioInprocessDevice(phys_mem_, 0 /* device_features */),
        queue_fake_(queue(), 16 /* queue_size */) {}

  VirtioQueue* queue() { return VirtioInprocessDevice::queue(0); }
  VirtioQueueFake* queue_fake() { return &queue_fake_; }

 private:
  PhysMemFake phys_mem_;
  VirtioQueueFake queue_fake_;
};

#endif  // GARNET_BIN_GUEST_VMM_VIRTIO_DEVICE_FAKE_H_

// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//
// This is the "basic" fence pool implementation.
//
// A host-OS-optimized platform will work directly with the VkFence
// payloads to avoid scanning for signaled fences.
//

#include "queue_pool.h"
#include "device.h"
#include "common/vk/vk_assert.h"

//
//
//

struct spn_queue_pool
{
  uint32_t qc;
};

//
//
//

void
spn_device_queue_pool_create(struct spn_device * const device,
                             uint32_t            const queue_count)
{
  device->queue_pool =
    spn_allocator_host_perm_alloc(&device->allocator.host.perm,
                                  SPN_MEM_FLAGS_READ_WRITE,
                                  sizeof(*device->queue_pool));

  device->queue_pool->qc = queue_count;
}

void
spn_device_queue_pool_dispose(struct spn_device * const device)
{
  spn_allocator_host_perm_free(&device->allocator.host.perm,
                               device->queue_pool);
}

//
//


VkQueue
spn_device_queue_pool_acquire(struct spn_device * const device)
{
  VkQueue q;

  vkGetDeviceQueue(device->vk->d,
                   device->vk->qfi,
                   0, // for now just return queue 0
                   &q);
  return q;
}

void
spn_device_queue_pool_release(struct spn_device * const device,
                              VkQueue             const queue)
{
  ; // NO-OP for now
}

//
//
//

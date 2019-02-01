// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>

#include <zircon/types.h>

extern zx_status_t qmi_fake_bind(void* ctx, zx_device_t* device);

static zx_driver_ops_t qmi_fake_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = qmi_fake_bind,
};

// clang-format off
ZIRCON_DRIVER_BEGIN(qmi_fake, qmi_fake_driver_ops, "zircon", "0.1", 2)
  BI_ABORT_IF_AUTOBIND,
  BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_TEST),
ZIRCON_DRIVER_END(qmi_fake)

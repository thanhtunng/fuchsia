// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>
#include <string>
#include <utility>

#include <fbl/vector.h>
#include <fuchsia/device/test/cpp/fidl.h>
#include <lib/devmgr-integration-test/fixture.h>
#include <lib/fidl/cpp/message.h>
#include <lib/zx/channel.h>

#include "mock-device.h"
#include "mock-device-hooks.h"

namespace libdriver_integration_test {

// Represents the first device that is offered for binding.  The only hook that
// will be called on it is the bind hook, and that will happen once.
class RootMockDevice {
private:
    using IsolatedDevmgr = devmgr_integration_test::IsolatedDevmgr;
public:
    // This constructor is public because make_unique wants it.  You probably
    // want Create().
    RootMockDevice(std::unique_ptr<MockDeviceHooks> hooks,
                   fidl::InterfacePtr<fuchsia::device::test::Device> test_device,
                   fidl::InterfaceRequest<fuchsia::device::mock::MockDevice> controller,
                   async_dispatcher_t* dispatcher, std::string path);
    ~RootMockDevice();

    static zx_status_t Create(const std::unique_ptr<IsolatedDevmgr>& devmgr,
                              async_dispatcher_t* dispatcher,
                              std::unique_ptr<MockDeviceHooks> hooks,
                              std::unique_ptr<RootMockDevice>* mock_out);

    // Path to device relative to the devmgr's devfs.
    const std::string& path() const { return path_; }
private:
    // Control interface for the root device in the test.  We use this to
    // trigger tree tear-down in tests.
    fidl::InterfacePtr<fuchsia::device::test::Device> test_device_;
    std::string path_;
    // Use the normal MockDevice internally to share logic
    MockDevice mock_;
};

} // namespace libdriver_integration_test

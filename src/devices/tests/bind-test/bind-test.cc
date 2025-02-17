// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.device.test/cpp/wire.h>
#include <fidl/fuchsia.device/cpp/wire.h>
#include <fuchsia/driver/development/cpp/fidl.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/driver.h>
#include <lib/devmgr-integration-test/fixture.h>
#include <lib/fdio/directory.h>
#include <lib/sys/cpp/component_context.h>

#include <gtest/gtest.h>

#include "src/lib/fxl/strings/string_printf.h"

const std::string kDevPrefix = "/dev/";
const std::string kDriverTestDir = "/boot/driver/test";
const std::string kDriverLibname = "bind-test-driver.so";
const std::string kChildDeviceName = "child";

using devmgr_integration_test::IsolatedDevmgr;

class BindCompilerTest : public testing::Test {
 protected:
  void SetUp() override {
    auto args = IsolatedDevmgr::DefaultArgs();

    args.sys_device_driver = "/boot/driver/test-parent-sys.so";
    args.driver_search_paths.push_back("/boot/driver");
    args.driver_search_paths.push_back("/boot/driver/test");

    ASSERT_EQ(IsolatedDevmgr::Create(std::move(args), &devmgr_), ZX_OK);
    ASSERT_NE(devmgr_.svc_root_dir().channel(), ZX_HANDLE_INVALID);

    // Wait for /dev/sys/test/test to appear, then get a channel to it.
    fbl::unique_fd root_fd;
    zx_status_t status = devmgr_integration_test::RecursiveWaitForFile(devmgr_.devfs_root(),
                                                                       "sys/test/test", &root_fd);
    ASSERT_EQ(status, ZX_OK);

    auto root_device_endpoints = fidl::CreateEndpoints<fuchsia_device_test::RootDevice>();
    ASSERT_EQ(root_device_endpoints.status_value(), ZX_OK);

    auto root_device = fidl::BindSyncClient(std::move(root_device_endpoints->client));
    status = fdio_get_service_handle(root_fd.release(),
                                     root_device.mutable_channel()->reset_and_get_address());
    ASSERT_EQ(status, ZX_OK);

    auto endpoints = fidl::CreateEndpoints<fuchsia_device::Controller>();
    ASSERT_EQ(endpoints.status_value(), ZX_OK);

    // Create the root test device in /dev/sys/test/test, and get its relative path from /dev.
    auto result = root_device.CreateDevice(fidl::StringView::FromExternal(kDriverLibname),
                                           endpoints->server.TakeChannel());

    ASSERT_EQ(result.status(), ZX_OK);
    ASSERT_EQ(result->status, ZX_OK);

    ASSERT_GE(result->path.size(), kDevPrefix.size());
    ASSERT_EQ(strncmp(result->path.data(), kDevPrefix.c_str(), kDevPrefix.size()), 0);
    relative_device_path_ = std::string(result->path.data() + kDevPrefix.size(),
                                        result->path.size() - kDevPrefix.size());

    // Bind the test driver to the new device.
    driver_libpath_ = kDriverTestDir + "/" + kDriverLibname;
    auto response =
        fidl::WireCall(endpoints->client).Bind(::fidl::StringView::FromExternal(driver_libpath_));
    status = response.status();
    if (status == ZX_OK) {
      if (response->result.is_err()) {
        status = response->result.err();
      }
    }
    ASSERT_EQ(status, ZX_OK);

    // Connect to the DriverDevelopment service.
    auto bind_endpoints = fidl::CreateEndpoints<fuchsia::driver::development::DriverDevelopment>();
    ASSERT_EQ(bind_endpoints.status_value(), ZX_OK);

    std::string svc_name =
        fxl::StringPrintf("svc/%s", fuchsia::driver::development::DriverDevelopment::Name_);
    sys::ServiceDirectory svc_dir(devmgr_.TakeSvcRootDir().TakeChannel());
    status = svc_dir.Connect(svc_name, bind_endpoints->server.TakeChannel());
    ASSERT_EQ(status, ZX_OK);

    driver_dev_.Bind(bind_endpoints->client.TakeChannel());
  }

  IsolatedDevmgr devmgr_;
  fuchsia::driver::development::DriverDevelopmentSyncPtr driver_dev_;
  std::string driver_libpath_;
  std::string relative_device_path_;
};

// Check that calling GetDriverInfo with an invalid driver path returns ZX_ERR_NOT_FOUND.
TEST_F(BindCompilerTest, InvalidDriver) {
  fuchsia::driver::development::DriverIndex_GetDriverInfo_Result result;
  ASSERT_EQ(driver_dev_->GetDriverInfo({"abc"}, &result), ZX_OK);
  ASSERT_TRUE(result.is_err());
  ASSERT_EQ(result.err(), ZX_ERR_NOT_FOUND);
}

// Get the bind program of the test driver and check that it has the expected instructions.
TEST_F(BindCompilerTest, ValidDriver) {
  fuchsia::driver::development::DriverIndex_GetDriverInfo_Result result;
  ASSERT_EQ(driver_dev_->GetDriverInfo({driver_libpath_}, &result), ZX_OK);
  ASSERT_TRUE(result.is_response());
  ASSERT_EQ(result.response().drivers.size(), 1u);
  auto instructions = result.response().drivers[0].bind_rules().bytecode_v1();

  zx_bind_inst_t expected_instructions[] = {
      BI_ABORT_IF_AUTOBIND,
      BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_TEST),
      BI_MATCH(),
  };

  ASSERT_EQ(instructions.size(), countof(expected_instructions));
  for (size_t i = 0; i < instructions.size(); i++) {
    ASSERT_EQ(instructions[i].op, expected_instructions[i].op);
    ASSERT_EQ(instructions[i].arg, expected_instructions[i].arg);
  }
  ASSERT_EQ(instructions.size(), countof(expected_instructions));
}

// Check that calling GetDeviceInfo with an invalid device path returns ZX_ERR_NOT_FOUND.
TEST_F(BindCompilerTest, InvalidDevice) {
  fuchsia::driver::development::DriverDevelopment_GetDeviceInfo_Result result;
  ASSERT_EQ(driver_dev_->GetDeviceInfo({"abc"}, &result), ZX_OK);
  ASSERT_TRUE(result.is_err());
  ASSERT_EQ(result.err(), ZX_ERR_NOT_FOUND);
}

// Get the properties of the test driver's child device and check that they are as expected.
TEST_F(BindCompilerTest, ValidDevice) {
  std::string child_device_path(relative_device_path_ + "/" + kChildDeviceName);

  fuchsia::driver::development::DriverDevelopment_GetDeviceInfo_Result result;
  ASSERT_EQ(driver_dev_->GetDeviceInfo({child_device_path}, &result), ZX_OK);

  ASSERT_TRUE(result.is_response());
  auto props = result.response().devices[0].property_list().props;

  zx_device_prop_t expected_props[] = {
      {BIND_PROTOCOL, 0, ZX_PROTOCOL_PCI},
      {BIND_PCI_VID, 0, 1234},
      {BIND_PCI_DID, 0, 1234},
  };

  ASSERT_EQ(props.size(), countof(expected_props));
  for (size_t i = 0; i < props.size(); i++) {
    ASSERT_EQ(props[i].id, expected_props[i].id);
    ASSERT_EQ(props[i].reserved, expected_props[i].reserved);
    ASSERT_EQ(props[i].value, expected_props[i].value);
  }
}

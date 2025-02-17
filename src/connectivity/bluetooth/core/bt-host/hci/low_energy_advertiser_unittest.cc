// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/bluetooth/core/bt-host/hci/low_energy_advertiser.h"

#include "src/connectivity/bluetooth/core/bt-host/common/test_helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/extended_low_energy_advertiser.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/legacy_low_energy_advertiser.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/controller_test.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/fake_controller.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/fake_peer.h"

// LowEnergyAdvertiser has many potential subclasses (e.g. LegacyLowEnergyAdvertiser,
// ExtendedLowEnergyAdvertiser, etc). The unique features of these subclass are tested
// individually in their own unittest files. However, there are some common features that all
// LowEnergyAdvertisers should follow. This class implements a type parameterized test to
// exercise those common features.
//
// If you add a new subclass of LowEnergyAdvertiser in the future, make sure to add its type to
// the list of types below (in the TYPED_TEST_SUITE) so that its common features are exercised
// as well.

namespace bt::hci {
namespace {

using testing::FakeController;
using testing::FakePeer;

using AdvertisingOptions = LowEnergyAdvertiser::AdvertisingOptions;
using TestingBase = bt::testing::ControllerTest<FakeController>;

constexpr ConnectionHandle kConnectionHandle = 0x0001;
constexpr AdvertisingIntervalRange kTestInterval(kLEAdvertisingIntervalMin,
                                                 kLEAdvertisingIntervalMax);

const DeviceAddress kPublicAddress(DeviceAddress::Type::kLEPublic, {1});
const DeviceAddress kRandomAddress(DeviceAddress::Type::kLERandom, {2});

// Various parts of the Bluetooth Core Spec require that advertising interval min and max are not
// the same value. We shouldn't allow it either. For example, Core Spec Volume 4, Part E, Section
// 7.8.5: "The Advertising_Interval_Min and Advertising_Interval_Max should not be the same value
// to enable the Controller to determine the best advertising interval given other activities."
TEST(AdvertisingIntervalRangeDeathTest, MaxMinNotSame) {
  EXPECT_DEATH(AdvertisingIntervalRange(kLEAdvertisingIntervalMin, kLEAdvertisingIntervalMin),
               ".*");
}

TEST(AdvertisingIntervalRangeDeathTest, MinLessThanMax) {
  EXPECT_DEATH(AdvertisingIntervalRange(kLEAdvertisingIntervalMax, kLEAdvertisingIntervalMin),
               ".*");
}

template <typename T>
class LowEnergyAdvertiserTest : public TestingBase {
 public:
  LowEnergyAdvertiserTest() = default;
  ~LowEnergyAdvertiserTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();

    // ACL data channel needs to be present for production hci::Connection objects.
    TestingBase::InitializeACLDataChannel(hci::DataBufferInfo(),
                                          hci::DataBufferInfo(hci::kMaxACLPayloadSize, 10));

    FakeController::Settings settings;
    settings.ApplyLegacyLEConfig();
    settings.bd_addr = kPublicAddress;
    test_device()->set_settings(settings);

    advertiser_ = std::make_unique<T>(transport()->WeakPtr());

    test_device()->set_num_supported_advertising_sets(0xEF);
    test_device()->StartCmdChannel(test_cmd_chan());
    test_device()->StartAclChannel(test_acl_chan());
  }

  void TearDown() override {
    advertiser_ = nullptr;
    test_device()->Stop();
    TestingBase::TearDown();
  }

  LowEnergyAdvertiser* advertiser() const { return advertiser_.get(); }

  StatusCallback GetSuccessCallback() {
    return [this](Status status) {
      last_status_ = status;
      EXPECT_TRUE(status) << status.ToString();
    };
  }

  StatusCallback GetErrorCallback() {
    return [this](Status status) {
      last_status_ = status;
      EXPECT_FALSE(status);
    };
  }

  std::optional<Status> GetLastStatus() {
    if (!last_status_) {
      return std::nullopt;
    }

    Status status = last_status_.value();
    last_status_.reset();
    return status;
  }

  // Makes some fake advertising data.
  // |include_flags| signals whether to include flag encoding size in the data calculation.
  AdvertisingData GetExampleData(bool include_flags = true) {
    AdvertisingData result;

    auto name = "fuchsia";
    EXPECT_TRUE(result.SetLocalName(name));

    auto appearance = 0x1234;
    result.SetAppearance(appearance);

    EXPECT_LE(result.CalculateBlockSize(include_flags), kMaxLEAdvertisingDataLength);
    return result;
  }

  // Makes fake advertising data that is too large.
  // |include_flags| signals whether to include flag encoding size in the data calculation.
  AdvertisingData GetTooLargeExampleData(bool include_flags,
                                         std::size_t size = kMaxLEAdvertisingDataLength + 1) {
    AdvertisingData result;

    if (include_flags) {
      size -= kTLVFlagsSize;
    }

    std::ostringstream oss;
    for (unsigned int i = 0; i <= size; i++) {
      oss << 'a';
    }

    EXPECT_TRUE(result.SetLocalName(oss.str()));

    // The maximum advertisement packet is: |kMaxLEAdvertisingDataLength| = 31, and |result| = 32
    // bytes. |result| should be too large to advertise.
    EXPECT_GT(result.CalculateBlockSize(include_flags), kMaxLEAdvertisingDataLength);
    return result;
  }

  std::optional<AdvertisingHandle> CurrentAdvertisingHandle() const {
    if (std::is_same_v<T, ExtendedLowEnergyAdvertiser>) {
      auto extended = static_cast<ExtendedLowEnergyAdvertiser*>(advertiser());
      return extended->LastUsedHandleForTesting();
    }

    return 0;  // non-extended advertising doesn't use handles, we can return any value
  }

  const FakeController::LEAdvertisingState& GetControllerAdvertisingState() const {
    if (std::is_same_v<T, LegacyLowEnergyAdvertiser>) {
      return test_device()->legacy_advertising_state();
    }

    if (std::is_same_v<T, ExtendedLowEnergyAdvertiser>) {
      std::optional<AdvertisingHandle> handle = CurrentAdvertisingHandle();
      if (!handle) {
        static FakeController::LEAdvertisingState empty;
        return empty;
      }

      return test_device()->extended_advertising_state(handle.value());
    }

    EXPECT_TRUE(false) << "advertiser is of unknown type";

    // return something in order to compile, tests will fail if they get here
    static FakeController::LEAdvertisingState state;
    return state;
  }

 private:
  std::unique_ptr<LowEnergyAdvertiser> advertiser_;
  std::optional<Status> last_status_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyAdvertiserTest);
};

using Implementations = ::testing::Types<LegacyLowEnergyAdvertiser, ExtendedLowEnergyAdvertiser>;
TYPED_TEST_SUITE(LowEnergyAdvertiserTest, Implementations);

// - Stops the advertisement when an incoming connection comes in
// - Possible to restart advertising
// - Advertising state cleaned up between calls
TYPED_TEST(LowEnergyAdvertiserTest, ConnectionTest) {
  AdvertisingData adv_data = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/false);

  ConnectionPtr link;
  auto conn_cb = [&link](auto cb_link) { link = std::move(cb_link); };

  // Start advertising kPublicAddress
  this->advertiser()->StartAdvertising(kPublicAddress, adv_data, scan_data, options, conn_cb,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();
  EXPECT_TRUE(this->GetLastStatus());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(kPublicAddress));

  // Accept a connection and ensure that connection state is set up correctly
  link.reset();
  this->advertiser()->OnIncomingConnection(kConnectionHandle, Connection::Role::kSlave,
                                           kRandomAddress, LEConnectionParameters());
  std::optional<AdvertisingHandle> handle = this->CurrentAdvertisingHandle();
  ASSERT_TRUE(handle);
  this->test_device()->SendLEAdvertisingSetTerminatedEvent(kConnectionHandle, handle.value());
  this->RunLoopUntilIdle();

  ASSERT_TRUE(link);
  EXPECT_EQ(kConnectionHandle, link->handle());
  EXPECT_EQ(kPublicAddress, link->local_address());
  EXPECT_EQ(kRandomAddress, link->peer_address());
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
  EXPECT_FALSE(this->advertiser()->IsAdvertising(kPublicAddress));

  // Advertising state should get cleared on a disconnection
  link->Disconnect(StatusCode::kRemoteUserTerminatedConnection);
  this->test_device()->SendDisconnectionCompleteEvent(link->handle());
  this->RunLoopUntilIdle();
  EXPECT_FALSE(this->GetControllerAdvertisingState().enabled);

  // Restart advertising using a different local address
  this->advertiser()->StartAdvertising(kRandomAddress, adv_data, scan_data, options, conn_cb,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();
  EXPECT_TRUE(this->GetLastStatus());
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);

  // Accept a connection from kPublicAddress. The internal advertising state should get assigned
  // correctly with no remnants of the previous advertise.
  link.reset();
  this->advertiser()->OnIncomingConnection(kConnectionHandle, Connection::Role::kSlave,
                                           kPublicAddress, LEConnectionParameters());
  handle = this->CurrentAdvertisingHandle();
  ASSERT_TRUE(handle);
  this->test_device()->SendLEAdvertisingSetTerminatedEvent(kConnectionHandle, handle.value());
  this->RunLoopUntilIdle();

  ASSERT_TRUE(link);
  EXPECT_EQ(kRandomAddress, link->local_address());
  EXPECT_EQ(kPublicAddress, link->peer_address());
}

// Tests that advertising can be restarted right away in a connection callback.
TYPED_TEST(LowEnergyAdvertiserTest, RestartInConnectionCallback) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/false);

  ConnectionPtr link;
  auto conn_cb = [&, this](auto cb_link) {
    link = std::move(cb_link);

    this->advertiser()->StartAdvertising(
        kPublicAddress, ad, scan_data, options, [](ConnectionPtr) { /*noop*/ },
        this->GetSuccessCallback());
  };

  this->advertiser()->StartAdvertising(kPublicAddress, ad, scan_data, options, conn_cb,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();
  EXPECT_TRUE(this->GetLastStatus());
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);

  bool enabled = true;
  std::vector<bool> adv_states;
  this->test_device()->set_advertising_state_callback([this, &adv_states, &enabled] {
    bool new_enabled = this->GetControllerAdvertisingState().enabled;
    if (enabled != new_enabled) {
      adv_states.push_back(new_enabled);
      enabled = new_enabled;
    }
  });

  this->advertiser()->OnIncomingConnection(kConnectionHandle, Connection::Role::kSlave,
                                           kRandomAddress, LEConnectionParameters());
  std::optional<AdvertisingHandle> handle = this->CurrentAdvertisingHandle();
  ASSERT_TRUE(handle);
  this->test_device()->SendLEAdvertisingSetTerminatedEvent(kConnectionHandle, handle.value());

  // Advertising should get disabled and re-enabled.
  this->RunLoopUntilIdle();
  ASSERT_EQ(2u, adv_states.size());
  EXPECT_FALSE(adv_states[0]);
  EXPECT_TRUE(adv_states[1]);
}

// An incoming connection when not advertising should get disconnected.
TYPED_TEST(LowEnergyAdvertiserTest, IncomingConnectionWhenNotAdvertising) {
  std::vector<std::pair<bool, ConnectionHandle>> connection_states;
  this->test_device()->set_connection_state_callback(
      [&](const auto& address, auto handle, bool connected, bool canceled) {
        EXPECT_EQ(kRandomAddress, address);
        EXPECT_FALSE(canceled);
        connection_states.push_back(std::make_pair(connected, handle));
      });

  auto fake_peer = std::make_unique<FakePeer>(kRandomAddress, true, true);
  this->test_device()->AddPeer(std::move(fake_peer));
  this->test_device()->ConnectLowEnergy(kRandomAddress, ConnectionRole::kSlave);
  this->RunLoopUntilIdle();

  ASSERT_EQ(1u, connection_states.size());
  auto [connection_state, handle] = connection_states[0];
  EXPECT_TRUE(connection_state);

  // Notify the advertiser of the incoming connection. It should reject it and the controller
  // should become disconnected.
  this->advertiser()->OnIncomingConnection(handle, Connection::Role::kSlave, kRandomAddress,
                                           LEConnectionParameters());
  this->test_device()->SendLEAdvertisingSetTerminatedEvent(kConnectionHandle, 0);
  this->RunLoopUntilIdle();
  ASSERT_EQ(2u, connection_states.size());
  auto [connection_state_after_disconnect, disconnected_handle] = connection_states[1];
  EXPECT_EQ(handle, disconnected_handle);
  EXPECT_FALSE(connection_state_after_disconnect);
}

// An incoming connection during non-connectable advertising should get disconnected.
TYPED_TEST(LowEnergyAdvertiserTest, IncomingConnectionWhenNonConnectableAdvertising) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kPublicAddress, ad, scan_data, options, nullptr,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();
  ASSERT_TRUE(this->GetLastStatus());

  std::vector<std::pair<bool, ConnectionHandle>> connection_states;
  this->test_device()->set_connection_state_callback(
      [&](const auto& address, auto handle, bool connected, bool canceled) {
        EXPECT_EQ(kRandomAddress, address);
        EXPECT_FALSE(canceled);
        connection_states.push_back(std::make_pair(connected, handle));
      });

  auto fake_peer = std::make_unique<FakePeer>(kRandomAddress, true, true);
  this->test_device()->AddPeer(std::move(fake_peer));
  this->test_device()->ConnectLowEnergy(kRandomAddress, ConnectionRole::kSlave);
  this->RunLoopUntilIdle();

  ASSERT_EQ(1u, connection_states.size());
  auto [connection_state, handle] = connection_states[0];
  EXPECT_TRUE(connection_state);

  // Notify the advertiser of the incoming connection. It should reject it and the controller
  // should become disconnected.
  this->advertiser()->OnIncomingConnection(handle, Connection::Role::kSlave, kRandomAddress,
                                           LEConnectionParameters());
  this->test_device()->SendLEAdvertisingSetTerminatedEvent(kConnectionHandle, 0);
  this->RunLoopUntilIdle();
  ASSERT_EQ(2u, connection_states.size());
  auto [connection_state_after_disconnect, disconnected_handle] = connection_states[1];
  EXPECT_EQ(handle, disconnected_handle);
  EXPECT_FALSE(connection_state_after_disconnect);
}

// Tests starting and stopping an advertisement.
TYPED_TEST(LowEnergyAdvertiserTest, StartAndStop) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(kRandomAddress, ad, scan_data, options, nullptr,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();
  EXPECT_TRUE(this->GetLastStatus());
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);

  this->advertiser()->StopAdvertising(kRandomAddress);
  this->RunLoopUntilIdle();
  EXPECT_FALSE(this->GetControllerAdvertisingState().enabled);
}

// Tests that an advertisement is configured with the correct parameters.
TYPED_TEST(LowEnergyAdvertiserTest, AdvertisingParameters) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  auto flags = AdvFlag::kLEGeneralDiscoverableMode;
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, flags,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(kRandomAddress, ad, scan_data, options, nullptr,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();
  EXPECT_TRUE(this->GetLastStatus());

  // The expected advertisement including the Flags.
  DynamicByteBuffer expected_ad(ad.CalculateBlockSize(/*include_flags=*/true));
  ad.WriteBlock(&expected_ad, flags);

  DynamicByteBuffer expected_scan_data(scan_data.CalculateBlockSize(/*include_flags=*/false));
  scan_data.WriteBlock(&expected_scan_data, std::nullopt);

  std::optional<FakeController::LEAdvertisingState> state = this->GetControllerAdvertisingState();
  EXPECT_TRUE(state);
  EXPECT_TRUE(state->enabled);
  EXPECT_EQ(kTestInterval.min(), state->interval_min);
  EXPECT_EQ(kTestInterval.max(), state->interval_max);
  EXPECT_EQ(expected_ad, state->advertised_view());
  EXPECT_EQ(expected_scan_data, state->scan_rsp_view());
  EXPECT_EQ(hci::LEOwnAddressType::kRandom, state->own_address_type);

  // Restart advertising with a public address and verify that the configured
  // local address type is correct.
  this->advertiser()->StopAdvertising(kRandomAddress);
  AdvertisingOptions new_options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                                 /*include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kPublicAddress, ad, scan_data, new_options, nullptr,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();
  EXPECT_TRUE(this->GetLastStatus());

  state = this->GetControllerAdvertisingState();
  EXPECT_TRUE(state);
  EXPECT_TRUE(state->enabled);
  EXPECT_EQ(hci::LEOwnAddressType::kPublic, state->own_address_type);
}

// Tests that advertising interval values are capped within the allowed range.
TYPED_TEST(LowEnergyAdvertiserTest, AdvertisingIntervalWithinAllowedRange) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();

  // Pass min and max values that are outside the allowed range. These should be capped.
  constexpr AdvertisingIntervalRange interval(kLEAdvertisingIntervalMin - 1,
                                              kLEAdvertisingIntervalMax + 1);
  AdvertisingOptions options(interval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kRandomAddress, ad, scan_data, options, nullptr,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();
  EXPECT_TRUE(this->GetLastStatus());

  std::optional<FakeController::LEAdvertisingState> state = this->GetControllerAdvertisingState();
  EXPECT_TRUE(state);
  EXPECT_EQ(kLEAdvertisingIntervalMin, state->interval_min);
  EXPECT_EQ(kLEAdvertisingIntervalMax, state->interval_max);

  // Reconfigure with values that are within the range. These should get passed down as is.
  const AdvertisingIntervalRange new_interval(kLEAdvertisingIntervalMin + 1,
                                              kLEAdvertisingIntervalMax - 1);
  AdvertisingOptions new_options(new_interval, /*anonymous=*/false, kDefaultNoAdvFlags,
                                 /*include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kRandomAddress, ad, scan_data, new_options, nullptr,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();
  EXPECT_TRUE(this->GetLastStatus());

  state = this->GetControllerAdvertisingState();
  EXPECT_TRUE(state);
  EXPECT_EQ(new_interval.min(), state->interval_min);
  EXPECT_EQ(new_interval.max(), state->interval_max);
}

TYPED_TEST(LowEnergyAdvertiserTest, StartWhileStarting) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  DeviceAddress addr = kRandomAddress;

  const AdvertisingIntervalRange old_interval = kTestInterval;
  AdvertisingOptions old_options(old_interval, /*anonymous=*/false, kDefaultNoAdvFlags,
                                 /*include_tx_power_level=*/false);
  const AdvertisingIntervalRange new_interval(kTestInterval.min() + 1, kTestInterval.max() - 1);
  AdvertisingOptions new_options(new_interval, /*anonymous=*/false, kDefaultNoAdvFlags,
                                 /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(addr, ad, scan_data, old_options, nullptr, [](auto) {});
  EXPECT_FALSE(this->GetControllerAdvertisingState().enabled);

  // This call should override the previous call and succeed with the new parameters.
  this->advertiser()->StartAdvertising(addr, ad, scan_data, new_options, nullptr,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();
  EXPECT_TRUE(this->GetLastStatus());
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);
  EXPECT_EQ(new_interval.max(), this->GetControllerAdvertisingState().interval_max);
}

TYPED_TEST(LowEnergyAdvertiserTest, StartWhileStopping) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  DeviceAddress addr = kRandomAddress;
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/false);

  // Get to a started state.
  this->advertiser()->StartAdvertising(addr, ad, scan_data, options, nullptr,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();
  EXPECT_TRUE(this->GetLastStatus());
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);

  // Initiate a request to Stop and wait until it's partially in progress.
  bool enabled = true;
  bool was_disabled = false;
  auto adv_state_cb = [&] {
    enabled = this->GetControllerAdvertisingState().enabled;
    if (!was_disabled && !enabled) {
      was_disabled = true;

      // Starting now should cancel the stop sequence and succeed.
      this->advertiser()->StartAdvertising(addr, ad, scan_data, options, nullptr,
                                           this->GetSuccessCallback());
    }
  };
  this->test_device()->set_advertising_state_callback(adv_state_cb);

  this->advertiser()->StopAdvertising(addr);

  // Advertising should have been momentarily disabled.
  this->RunLoopUntilIdle();
  EXPECT_TRUE(was_disabled);
  EXPECT_TRUE(enabled);
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);
}

// - StopAdvertisement noops when the advertisement address is wrong
// - Sets the advertisement data to null when stopped to prevent data leakage
//   (re-enable advertising without changing data, intercept)
TYPED_TEST(LowEnergyAdvertiserTest, StopAdvertisingConditions) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(kRandomAddress, ad, scan_data, options, nullptr,
                                       this->GetSuccessCallback());

  this->RunLoopUntilIdle();

  EXPECT_TRUE(this->GetLastStatus());

  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);
  DynamicByteBuffer expected_ad(ad.CalculateBlockSize(/*include_flags=*/true));
  ad.WriteBlock(&expected_ad, kDefaultNoAdvFlags);
  EXPECT_TRUE(
      ContainersEqual(this->GetControllerAdvertisingState().advertised_view(), expected_ad));

  this->RunLoopUntilIdle();
  this->advertiser()->StopAdvertising(kPublicAddress);
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);
  EXPECT_TRUE(
      ContainersEqual(this->GetControllerAdvertisingState().advertised_view(), expected_ad));

  this->advertiser()->StopAdvertising(kRandomAddress);

  this->RunLoopUntilIdle();
  EXPECT_FALSE(this->GetControllerAdvertisingState().enabled);
  EXPECT_EQ(0u, this->GetControllerAdvertisingState().advertised_view().size());
  EXPECT_EQ(0u, this->GetControllerAdvertisingState().scan_rsp_view().size());
}

// - Updates data and params for the same address when advertising already
TYPED_TEST(LowEnergyAdvertiserTest, AdvertiseUpdate) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(kRandomAddress, ad, scan_data, options, nullptr,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();

  EXPECT_TRUE(this->GetLastStatus());
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);

  // The expected advertising data payload, with the flags.
  DynamicByteBuffer expected_ad(ad.CalculateBlockSize(/*include_flags=*/true));
  ad.WriteBlock(&expected_ad, kDefaultNoAdvFlags);
  EXPECT_TRUE(
      ContainersEqual(this->GetControllerAdvertisingState().advertised_view(), expected_ad));

  EXPECT_EQ(kTestInterval.min(), this->GetControllerAdvertisingState().interval_min);
  EXPECT_EQ(kTestInterval.max(), this->GetControllerAdvertisingState().interval_max);

  uint16_t new_appearance = 0x6789;
  ad.SetAppearance(new_appearance);

  const AdvertisingIntervalRange new_interval(kTestInterval.min() + 1, kTestInterval.max() - 1);
  AdvertisingOptions new_options(new_interval, /*anonymous=*/false, kDefaultNoAdvFlags,
                                 /*include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kRandomAddress, ad, scan_data, new_options, nullptr,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();

  EXPECT_TRUE(this->GetLastStatus());
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);

  DynamicByteBuffer expected_new_ad(ad.CalculateBlockSize(/*include_flags=*/true));
  ad.WriteBlock(&expected_new_ad, kDefaultNoAdvFlags);
  EXPECT_TRUE(
      ContainersEqual(this->GetControllerAdvertisingState().advertised_view(), expected_new_ad));

  EXPECT_EQ(new_interval.min(), this->GetControllerAdvertisingState().interval_min);
  EXPECT_EQ(new_interval.max(), this->GetControllerAdvertisingState().interval_max);
}

// Ensures advertising set data is removed from controller memory after advertising is stopped
TYPED_TEST(LowEnergyAdvertiserTest, StopAdvertisingSingleAdvertisement) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();

  // start public address advertising
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kPublicAddress, ad, scan_data, options, nullptr,
                                       this->GetSuccessCallback());
  this->RunLoopUntilIdle();
  EXPECT_TRUE(this->GetLastStatus());

  constexpr uint8_t blank[kMaxLEAdvertisingDataLength] = {0};

  // check that advertiser and controller both report the same advertising state
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(kPublicAddress));

  {
    const FakeController::LEAdvertisingState& state = this->GetControllerAdvertisingState();
    EXPECT_TRUE(state.enabled);
    EXPECT_NE(0, std::memcmp(blank, state.data, kMaxLEAdvertisingDataLength));
    EXPECT_NE(0, state.data_length);
    EXPECT_NE(0, std::memcmp(blank, state.data, kMaxLEAdvertisingDataLength));
    EXPECT_NE(0, state.scan_rsp_length);
  }

  // stop advertising the random address
  this->advertiser()->StopAdvertising(kPublicAddress);
  this->RunLoopUntilIdle();

  // check that advertiser and controller both report the same advertising state
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
  EXPECT_FALSE(this->advertiser()->IsAdvertising(kPublicAddress));

  {
    const FakeController::LEAdvertisingState& state = this->GetControllerAdvertisingState();
    EXPECT_FALSE(state.enabled);
    EXPECT_EQ(0, std::memcmp(blank, state.data, kMaxLEAdvertisingDataLength));
    EXPECT_EQ(0, state.data_length);
    EXPECT_EQ(0, std::memcmp(blank, state.data, kMaxLEAdvertisingDataLength));
    EXPECT_EQ(0, state.scan_rsp_length);
  }
}

// - Rejects anonymous advertisement (unsupported)
TYPED_TEST(LowEnergyAdvertiserTest, NoAnonymous) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval, /*anonymous=*/true, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(kRandomAddress, ad, scan_data, options, nullptr,
                                       this->GetErrorCallback());
  EXPECT_TRUE(this->GetLastStatus());
  EXPECT_FALSE(this->GetControllerAdvertisingState().enabled);
}

TYPED_TEST(LowEnergyAdvertiserTest, AdvertisingDataTooLong) {
  AdvertisingData invalid_ad = this->GetTooLargeExampleData(/*include_flags=*/true);
  AdvertisingData valid_scan_rsp = this->GetExampleData(/*include_flags=*/false);
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/false);

  // Advertising data too large.
  this->advertiser()->StartAdvertising(kRandomAddress, invalid_ad, valid_scan_rsp, options, nullptr,
                                       this->GetErrorCallback());
  this->RunLoopUntilIdle();
  auto status = this->GetLastStatus();
  ASSERT_TRUE(status);
  EXPECT_EQ(HostError::kAdvertisingDataTooLong, status->error());
}

TYPED_TEST(LowEnergyAdvertiserTest, AdvertisingDataTooLongWithTxPower) {
  AdvertisingData invalid_ad = this->GetTooLargeExampleData(
      /*include_flags=*/true, kMaxLEAdvertisingDataLength - kTLVTxPowerLevelSize + 1);
  AdvertisingData valid_scan_rsp = this->GetExampleData(/*include_flags=*/false);
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/true);

  // Advertising data too large.
  this->advertiser()->StartAdvertising(kRandomAddress, invalid_ad, valid_scan_rsp, options, nullptr,
                                       this->GetErrorCallback());
  this->RunLoopUntilIdle();
  auto status = this->GetLastStatus();
  ASSERT_TRUE(status);
  EXPECT_EQ(HostError::kAdvertisingDataTooLong, status->error());
}

TYPED_TEST(LowEnergyAdvertiserTest, ScanResponseTooLong) {
  AdvertisingData valid_ad = this->GetExampleData();
  AdvertisingData invalid_scan_rsp = this->GetTooLargeExampleData(/*include_flags=*/false);
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kRandomAddress, valid_ad, invalid_scan_rsp, options, nullptr,
                                       this->GetErrorCallback());
  this->RunLoopUntilIdle();
  auto status = this->GetLastStatus();
  ASSERT_TRUE(status);
  EXPECT_EQ(HostError::kScanResponseTooLong, status->error());
}

TYPED_TEST(LowEnergyAdvertiserTest, ScanResponseTooLongWithTxPower) {
  AdvertisingData valid_ad = this->GetExampleData();
  AdvertisingData invalid_scan_rsp = this->GetTooLargeExampleData(
      /*include_flags=*/false, kMaxLEAdvertisingDataLength - kTLVTxPowerLevelSize + 1);
  AdvertisingOptions options(kTestInterval, /*anonymous=*/false, kDefaultNoAdvFlags,
                             /*include_tx_power_level=*/true);
  this->advertiser()->StartAdvertising(kRandomAddress, valid_ad, invalid_scan_rsp, options, nullptr,
                                       this->GetErrorCallback());
  this->RunLoopUntilIdle();
  auto status = this->GetLastStatus();
  ASSERT_TRUE(status);
  EXPECT_EQ(HostError::kScanResponseTooLong, status->error());
}

}  // namespace
}  // namespace bt::hci

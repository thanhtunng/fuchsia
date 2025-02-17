// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/bluetooth/core/bt-host/gap/bredr_discovery_manager.h"

#include "src/connectivity/bluetooth/core/bt-host/common/test_helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/peer_cache.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/controller_test.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/mock_controller.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_packets.h"

namespace bt::gap {
namespace {

using bt::testing::CommandTransaction;

using TestingBase = bt::testing::ControllerTest<bt::testing::MockController>;

// clang-format off
#define COMMAND_COMPLETE_RSP(opcode)                                         \
  CreateStaticByteBuffer(hci::kCommandCompleteEventCode, 0x04, 0xF0, \
                                 LowerBits((opcode)), UpperBits((opcode)),   \
                                 hci::kSuccess)

#define COMMAND_STATUS_RSP(opcode, statuscode)                       \
  CreateStaticByteBuffer(hci::kCommandStatusEventCode, 0x04, \
                                 (statuscode), 0xF0,                 \
                                 LowerBits((opcode)), UpperBits((opcode)))


const auto kWriteInquiryActivity = CreateStaticByteBuffer(
    LowerBits(hci::kWriteInquiryScanActivity), UpperBits(hci::kWriteInquiryScanActivity),
    0x04, // Param total size
    LowerBits(kInquiryScanInterval), UpperBits(kInquiryScanInterval),
    LowerBits(kInquiryScanWindow), UpperBits(kInquiryScanWindow)
);

const auto kWriteInquiryActivityRsp = COMMAND_COMPLETE_RSP(hci::kWriteInquiryScanActivity);

const auto kWriteInquiryType = CreateStaticByteBuffer(
    LowerBits(hci::kWriteInquiryScanType), UpperBits(hci::kWriteInquiryScanType),
    0x01, // Param total size
    0x01 // Interlaced Inquiry Scan
);

const auto kWriteInquiryTypeRsp = COMMAND_COMPLETE_RSP(hci::kWriteInquiryScanType);
// clang-format on

class BrEdrDiscoveryManagerTest : public TestingBase {
 public:
  BrEdrDiscoveryManagerTest() = default;
  ~BrEdrDiscoveryManagerTest() override = default;

  void SetUp() override {
    TestingBase::SetUp();

    test_device()->StartCmdChannel(test_cmd_chan());
    test_device()->StartAclChannel(test_acl_chan());

    NewDiscoveryManager(hci::InquiryMode::kStandard);
  }

  void TearDown() override {
    discovery_manager_ = nullptr;
    test_device()->Stop();
    TestingBase::TearDown();
  }

  void NewDiscoveryManager(hci::InquiryMode mode) {
    // We expect to set the Inquiry Scan and the Type when we start.
    EXPECT_CMD_PACKET_OUT(test_device(), kWriteInquiryActivity, &kWriteInquiryActivityRsp);
    EXPECT_CMD_PACKET_OUT(test_device(), kWriteInquiryType, &kWriteInquiryTypeRsp);

    discovery_manager_ =
        std::make_unique<BrEdrDiscoveryManager>(transport()->WeakPtr(), mode, &peer_cache_);

    RunLoopUntilIdle();
  }

  PeerCache* peer_cache() { return &peer_cache_; }

 protected:
  BrEdrDiscoveryManager* discovery_manager() const { return discovery_manager_.get(); }

 private:
  PeerCache peer_cache_;
  std::unique_ptr<BrEdrDiscoveryManager> discovery_manager_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(BrEdrDiscoveryManagerTest);
};

using GAP_BrEdrDiscoveryManagerTest = BrEdrDiscoveryManagerTest;

// Suffix DeathTest has GoogleTest-specific behavior
using BrEdrDiscoveryManagerDeathTest = BrEdrDiscoveryManagerTest;

// clang-format off
const auto kInquiry = CreateStaticByteBuffer(
  LowerBits(hci::kInquiry), UpperBits(hci::kInquiry),
  0x05, // Paramreter total size
  0x33, 0x8B, 0x9E, // GIAC
  0x08, // kInquiryLengthDefault
  0x00 // Unlimited responses
);

const auto kWriteLocalNameRsp =
    COMMAND_STATUS_RSP(hci::kWriteLocalName, hci::StatusCode::kSuccess);

const auto kWriteLocalNameRspError =
    COMMAND_STATUS_RSP(hci::kWriteLocalName, hci::StatusCode::kHardwareFailure);

const auto kWriteExtendedInquiryResponseRsp =
    COMMAND_STATUS_RSP(hci::kWriteExtendedInquiryResponse, hci::StatusCode::kSuccess);

const auto kWriteExtendedInquiryResponseRspError =
    COMMAND_STATUS_RSP(hci::kWriteExtendedInquiryResponse, hci::StatusCode::kHardwareFailure);

const auto kInquiryRsp = COMMAND_STATUS_RSP(hci::kInquiry, hci::kSuccess);

const auto kInquiryRspError = COMMAND_STATUS_RSP(hci::kInquiry, hci::kHardwareFailure);

const auto kInquiryComplete = CreateStaticByteBuffer(
  hci::kInquiryCompleteEventCode,
  0x01, // parameter_total_size (1 bytes)
  hci::kSuccess
);

const auto kInquiryCompleteError = CreateStaticByteBuffer(
  hci::kInquiryCompleteEventCode,
  0x01, // parameter_total_size (1 bytes)
  hci::kHardwareFailure
);

#define BD_ADDR(addr1) addr1, 0x00, 0x00, 0x00, 0x00, 0x00

const DeviceAddress kDeviceAddress1(DeviceAddress::Type::kBREDR,
                                    {BD_ADDR(0x01)});
const DeviceAddress kLeAliasAddress1(DeviceAddress::Type::kLEPublic,
                                     kDeviceAddress1.value());
const DeviceAddress kDeviceAddress2(DeviceAddress::Type::kBREDR,
                                    {BD_ADDR(0x02)});
const DeviceAddress kLeAliasAddress2(DeviceAddress::Type::kLEPublic,
                                     kDeviceAddress2.value());
const DeviceAddress kDeviceAddress3(DeviceAddress::Type::kBREDR,
                                    {BD_ADDR(0x03)});

const auto kInquiryResult = CreateStaticByteBuffer(
  hci::kInquiryResultEventCode,
  0x0F, // parameter_total_size (15 bytes)
  0x01, // num_responses
  BD_ADDR(0x01), // bd_addr[0]
  0x00, // page_scan_repetition_mode[0] (R0)
  0x00, // unused / reserved
  0x00, // unused / reserved
  0x00, 0x1F, 0x00, // class_of_device[0] (unspecified)
  0x00, 0x00 // clock_offset[0]
);

const StaticByteBuffer kInquiryResultIncompleteHeader(
  hci::kInquiryResultEventCode,
  0x00 // parameter_total_size (0 bytes)
  // truncated
);

const StaticByteBuffer kInquiryResultMissingResponses(
  hci::kInquiryResultEventCode,
  0x1D, // parameter_total_size (29 bytes)
  0x03, // num_responses (only two responses are packed)

  // first response
  BD_ADDR(0x01), // bd_addr[0]
  0x00, // page_scan_repetition_mode[0] (R0)
  0x00, // unused / reserved
  0x00, // unused / reserved
  0x00, 0x1F, 0x00, // class_of_device[0] (unspecified)
  0x00, 0x00, // clock_offset[0]

  // second response
  BD_ADDR(0x02), // bd_addr[0]
  0x00, // page_scan_repetition_mode[0] (R0)
  0x00, // unused / reserved
  0x00, // unused / reserved
  0x00, 0x1F, 0x00, // class_of_device[0] (unspecified)
  0x00, 0x00 // clock_offset[0]
);

const StaticByteBuffer kInquiryResultIncompleteResponse(
  hci::kInquiryResultEventCode,
  0x15, // parameter_total_size (21 bytes)
  0x02, // num_responses

  // first response
  BD_ADDR(0x01), // bd_addr[0]
  0x00, // page_scan_repetition_mode[0] (R0)
  0x00, // unused / reserved
  0x00, // unused / reserved
  0x00, 0x1F, 0x00, // class_of_device[0] (unspecified)
  0x00, 0x00, // clock_offset[0]

  // second response
  BD_ADDR(0x02) // bd_addr[0]
  // truncated
);

const auto kRSSIInquiryResult = CreateStaticByteBuffer(
  hci::kInquiryResultWithRSSIEventCode,
  0x0F, // parameter_total_size (15 bytes)
  0x01, // num_responses
  BD_ADDR(0x02), // bd_addr[0]
  0x00, // page_scan_repetition_mode[0] (R0)
  0x00, // unused / reserved
  0x00, 0x1F, 0x00, // class_of_device[0] (unspecified)
  0x00, 0x00, // clock_offset[0]
  0xEC // RSSI (-20dBm)
);

#define REMOTE_NAME_REQUEST(addr1) CreateStaticByteBuffer( \
    LowerBits(hci::kRemoteNameRequest), UpperBits(hci::kRemoteNameRequest), \
    0x0a, /* parameter_total_size (10 bytes) */ \
    BD_ADDR(addr1),  /* BD_ADDR */ \
    0x00, 0x00, 0x00, 0x80 /* page_scan_repetition_mode, 0, clock_offset */ \
);

const auto kRemoteNameRequest1 = REMOTE_NAME_REQUEST(0x01);
const auto kRemoteNameRequest2 = REMOTE_NAME_REQUEST(0x02);
const auto kRemoteNameRequest3 = REMOTE_NAME_REQUEST(0x03);

#undef REMOTE_NAME_REQUEST

const auto kRemoteNameRequestRsp =
    COMMAND_STATUS_RSP(hci::kRemoteNameRequest, hci::StatusCode::kSuccess);

#undef COMMAND_STATUS_RSP

const auto kRemoteNameRequestComplete1 = testing::RemoteNameRequestCompletePacket(
    kDeviceAddress1, {'F',    'u',    'c',    'h',    's',    'i',    'a',    '\xF0', '\x9F',
                      '\x92', '\x96', '\x00', '\x14', '\x15', '\x16', '\x17', '\x18', '\x19',
                      '\x1a', '\x1b', '\x1c', '\x1d', '\x1e', '\x1f', '\x20'}
    // remote name (Fuchsia💖)
    // Everything after the 0x00 should be ignored.
);
const auto kRemoteNameRequestComplete2 =
    testing::RemoteNameRequestCompletePacket(kDeviceAddress2, "Sapphire");

const auto kExtendedInquiryResult = CreateStaticByteBuffer(
  hci::kExtendedInquiryResultEventCode,
  0xFF, // parameter_total_size (255 bytes)
  0x01, // num_responses
  BD_ADDR(0x03),  // bd_addr
  0x00, // page_scan_repetition_mode (R0)
  0x00, // unused / reserved
  0x00, 0x1F, 0x00, // class_of_device (unspecified)
  0x00, 0x00, // clock_offset
  0xEC, // RSSI (-20dBm)
  // Extended Inquiry Response (240 bytes total)
  // Complete Local Name (12 bytes): Fuchsia 💖
  0x0C, 0x09, 'F', 'u', 'c', 'h', 's', 'i', 'a', 0xF0, 0x9F, 0x92, 0x96,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00
);

#undef BD_ADDR

const auto kInqCancel = CreateStaticByteBuffer(
  LowerBits(hci::kInquiryCancel), UpperBits(hci::kInquiryCancel),  // opcode
  0x00                                   // parameter_total_size
);

const auto kInqCancelRsp = COMMAND_COMPLETE_RSP(hci::kInquiryCancel);

const auto kSetExtendedMode = CreateStaticByteBuffer(
    LowerBits(hci::kWriteInquiryMode), UpperBits(hci::kWriteInquiryMode),
    0x01, // parameter_total_size
    0x02 // Extended Inquiry Result or Inquiry Result with RSSI
);

const auto kSetExtendedModeRsp = COMMAND_COMPLETE_RSP(hci::kWriteInquiryMode);

const auto kReadScanEnable = CreateStaticByteBuffer(
    LowerBits(hci::kReadScanEnable), UpperBits(hci::kReadScanEnable),
    0x00  // No parameters
);

const auto kWriteLocalName = CreateStaticByteBuffer(
  LowerBits(hci::kWriteLocalName), UpperBits(hci::kWriteLocalName),
  0xF8, // parameter_total_size (248 bytes)
  // Complete Local Name ()
  'A', 'B', 'C', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00
);

const auto kWriteExtendedInquiryResponse = CreateStaticByteBuffer(
  LowerBits(hci::kWriteExtendedInquiryResponse),
  UpperBits(hci::kWriteExtendedInquiryResponse),
  0xF1, // parameter_total_size (241 bytes)
  0x00, // fec_required
  0x04, // name_length + 1
  0x09, // DataType::kCompleteLocalName,
  // Complete Local Name (3 bytes + 1 byte null terminator + 234 bytes of zero padding)
  'A', 'B', 'C', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00
);

#define READ_SCAN_ENABLE_RSP(scan_enable)                                    \
  CreateStaticByteBuffer(hci::kCommandCompleteEventCode, 0x05, 0xF0, \
                                 LowerBits(hci::kReadScanEnable),            \
                                 UpperBits(hci::kReadScanEnable),            \
                                 hci::kSuccess, (scan_enable))

const auto kReadScanEnableRspNone = READ_SCAN_ENABLE_RSP(0x00);
const auto kReadScanEnableRspInquiry = READ_SCAN_ENABLE_RSP(0x01);
const auto kReadScanEnableRspPage = READ_SCAN_ENABLE_RSP(0x02);
const auto kReadScanEnableRspBoth = READ_SCAN_ENABLE_RSP(0x03);

#undef READ_SCAN_ENABLE_RSP

#define WRITE_SCAN_ENABLE_CMD(scan_enable)                               \
  CreateStaticByteBuffer(LowerBits(hci::kWriteScanEnable),       \
                                 UpperBits(hci::kWriteScanEnable), 0x01, \
                                 (scan_enable))

const auto kWriteScanEnableNone = WRITE_SCAN_ENABLE_CMD(0x00);
const auto kWriteScanEnableInq = WRITE_SCAN_ENABLE_CMD(0x01);
const auto kWriteScanEnablePage = WRITE_SCAN_ENABLE_CMD(0x02);
const auto kWriteScanEnableBoth = WRITE_SCAN_ENABLE_CMD(0x03);

#undef WRITE_SCAN_ENABLE_CMD

const auto kWriteScanEnableRsp = COMMAND_COMPLETE_RSP(hci::kWriteScanEnable);

#undef COMMAND_COMPLETE_RSP
// clang-format on

// Test: malformed inquiry result is fatal
TEST_F(BrEdrDiscoveryManagerDeathTest, MalformedInquiryResultFromControllerIsFatal) {
  EXPECT_CMD_PACKET_OUT(test_device(), kInquiry, &kInquiryRsp);

  std::unique_ptr<BrEdrDiscoverySession> session;

  discovery_manager()->RequestDiscovery([&session](auto status, auto cb_session) {
    EXPECT_TRUE(status);
    session = std::move(cb_session);
  });

  RunLoopUntilIdle();

  for (auto event : {kInquiryResultIncompleteHeader.view(), kInquiryResultMissingResponses.view(),
                     kInquiryResultIncompleteResponse.view()}) {
    EXPECT_DEATH_IF_SUPPORTED(
        [=] {
          test_device()->SendCommandChannelPacket(event);
          RunLoopUntilIdle();
        }(),
        ".*");
  }

  test_device()->SendCommandChannelPacket(kInquiryComplete);
}

// Test: discovering() answers correctly

// Test: requesting discovery should start inquiry
// Test: Inquiry Results that come in when there is discovery get reported up
// correctly to the sessions
// Test: Peers discovered are reported to the cache
// Test: Inquiry Results that come in when there's no discovery happening get
// discarded.
TEST_F(BrEdrDiscoveryManagerTest, RequestDiscoveryAndDrop) {
  EXPECT_CMD_PACKET_OUT(test_device(), kInquiry, &kInquiryRsp, &kInquiryResult);
  EXPECT_CMD_PACKET_OUT(test_device(), kRemoteNameRequest1, &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete1);

  std::unique_ptr<BrEdrDiscoverySession> session;
  size_t peers_found = 0u;

  discovery_manager()->RequestDiscovery([&session, &peers_found](auto status, auto cb_session) {
    EXPECT_TRUE(status);
    cb_session->set_result_callback([&peers_found](const auto&) { peers_found++; });
    session = std::move(cb_session);
  });

  EXPECT_FALSE(discovery_manager()->discovering());

  RunLoopUntilIdle();

  EXPECT_EQ(1u, peers_found);
  EXPECT_TRUE(discovery_manager()->discovering());

  EXPECT_CMD_PACKET_OUT(test_device(), kInquiry, &kInquiryRsp, &kInquiryResult);

  test_device()->SendCommandChannelPacket(kInquiryComplete);

  RunLoopUntilIdle();

  EXPECT_EQ(2u, peers_found);

  // TODO(jamuraa, fxbug.dev/668): test InquiryCancel when it is implemented

  session = nullptr;
  test_device()->SendCommandChannelPacket(kInquiryResult);

  RunLoopUntilIdle();

  EXPECT_EQ(2u, peers_found);
  EXPECT_FALSE(discovery_manager()->discovering());

  test_device()->SendCommandChannelPacket(kInquiryComplete);
}

// Test: requesting a second discovery should start a session without sending
// any more HCI commands.
// Test: dropping the first discovery shouldn't stop inquiry
// Test: starting two sessions at once should only start inquiry once
TEST_F(BrEdrDiscoveryManagerTest, MultipleRequests) {
  EXPECT_CMD_PACKET_OUT(test_device(), kInquiry, &kInquiryRsp, &kInquiryResult);
  EXPECT_CMD_PACKET_OUT(test_device(), kRemoteNameRequest1, &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete1);

  std::unique_ptr<BrEdrDiscoverySession> session1;
  size_t peers_found1 = 0u;

  discovery_manager()->RequestDiscovery([&session1, &peers_found1](auto status, auto cb_session) {
    EXPECT_TRUE(status);
    cb_session->set_result_callback([&peers_found1](const auto&) { peers_found1++; });
    session1 = std::move(cb_session);
  });

  EXPECT_FALSE(discovery_manager()->discovering());

  RunLoopUntilIdle();

  EXPECT_TRUE(session1);
  EXPECT_EQ(1u, peers_found1);
  EXPECT_TRUE(discovery_manager()->discovering());

  std::unique_ptr<BrEdrDiscoverySession> session2;
  size_t peers_found2 = 0u;

  discovery_manager()->RequestDiscovery([&session2, &peers_found2](auto status, auto cb_session) {
    EXPECT_TRUE(status);
    cb_session->set_result_callback([&peers_found2](const auto&) { peers_found2++; });
    session2 = std::move(cb_session);
  });

  RunLoopUntilIdle();

  EXPECT_TRUE(session2);
  EXPECT_EQ(1u, peers_found1);
  EXPECT_EQ(0u, peers_found2);
  EXPECT_TRUE(discovery_manager()->discovering());

  test_device()->SendCommandChannelPacket(kInquiryResult);

  RunLoopUntilIdle();

  EXPECT_EQ(2u, peers_found1);
  EXPECT_EQ(1u, peers_found2);

  session1 = nullptr;

  RunLoopUntilIdle();

  test_device()->SendCommandChannelPacket(kInquiryResult);

  RunLoopUntilIdle();

  EXPECT_EQ(2u, peers_found1);
  EXPECT_EQ(2u, peers_found2);

  // TODO(jamuraa, fxbug.dev/668): test InquiryCancel when it is implemented

  session2 = nullptr;

  test_device()->SendCommandChannelPacket(kInquiryResult);

  RunLoopUntilIdle();

  EXPECT_EQ(2u, peers_found1);
  EXPECT_EQ(2u, peers_found2);

  EXPECT_FALSE(discovery_manager()->discovering());

  test_device()->SendCommandChannelPacket(kInquiryComplete);

  RunLoopUntilIdle();
}

// Test: starting a session "while" the other one is stopping a session should
// still restart the Inquiry.
// Test: starting a session "while" the other one is stopping should return
// without needing an InquiryComplete first.
// Test: we should only request a peer's name if it's the first time we
// encounter it.
TEST_F(BrEdrDiscoveryManagerTest, RequestDiscoveryWhileStop) {
  EXPECT_CMD_PACKET_OUT(test_device(), kInquiry, &kInquiryRsp, &kInquiryResult);
  EXPECT_CMD_PACKET_OUT(test_device(), kRemoteNameRequest1, &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete1);

  std::unique_ptr<BrEdrDiscoverySession> session1;
  size_t peers_found1 = 0u;

  discovery_manager()->RequestDiscovery([&session1, &peers_found1](auto status, auto cb_session) {
    EXPECT_TRUE(status);
    cb_session->set_result_callback([&peers_found1](const auto&) { peers_found1++; });
    session1 = std::move(cb_session);
  });

  EXPECT_FALSE(discovery_manager()->discovering());

  RunLoopUntilIdle();

  EXPECT_TRUE(session1);
  EXPECT_EQ(1u, peers_found1);
  EXPECT_TRUE(discovery_manager()->discovering());

  // Drop the active session.
  session1 = nullptr;
  RunLoopUntilIdle();

  std::unique_ptr<BrEdrDiscoverySession> session2;
  size_t peers_found2 = 0u;
  discovery_manager()->RequestDiscovery([&session2, &peers_found2](auto status, auto cb_session) {
    EXPECT_TRUE(status);
    cb_session->set_result_callback([&peers_found2](const auto&) { peers_found2++; });
    session2 = std::move(cb_session);
  });

  // The new session should be started at this point, and inquiry results
  // returned.
  EXPECT_TRUE(session2);
  test_device()->SendCommandChannelPacket(kInquiryResult);

  RunLoopUntilIdle();

  EXPECT_EQ(1u, peers_found2);

  // Inquiry should be restarted when the Complete comes in because an active
  // session2 still exists.
  // TODO(jamuraa, fxbug.dev/668): test InquiryCancel when it is implemented
  EXPECT_CMD_PACKET_OUT(test_device(), kInquiry, &kInquiryRsp, &kInquiryResult);
  test_device()->SendCommandChannelPacket(kInquiryComplete);

  RunLoopUntilIdle();

  EXPECT_EQ(1u, peers_found1);
  EXPECT_EQ(2u, peers_found2);
  EXPECT_TRUE(discovery_manager()->discovering());

  test_device()->SendCommandChannelPacket(kInquiryResult);

  RunLoopUntilIdle();

  EXPECT_EQ(1u, peers_found1);
  EXPECT_EQ(3u, peers_found2);

  // TODO(jamuraa, fxbug.dev/668): test InquiryCancel when it is implemented
  session2 = nullptr;

  // After the session is dropped, even if another result comes in, no results
  // are sent to the callback.
  test_device()->SendCommandChannelPacket(kInquiryResult);

  RunLoopUntilIdle();

  EXPECT_EQ(1u, peers_found1);
  EXPECT_EQ(3u, peers_found2);
}

// Test: When Inquiry Fails to start, we report this back to the requester.
TEST_F(BrEdrDiscoveryManagerTest, RequestDiscoveryError) {
  EXPECT_CMD_PACKET_OUT(test_device(), kInquiry, &kInquiryRspError, &kInquiryResult);
  EXPECT_CMD_PACKET_OUT(test_device(), kRemoteNameRequest1, &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete1);

  std::unique_ptr<BrEdrDiscoverySession> session;

  discovery_manager()->RequestDiscovery([](auto status, auto cb_session) {
    EXPECT_FALSE(status);
    EXPECT_FALSE(cb_session);
    EXPECT_EQ(HostError::kProtocolError, status.error());
    EXPECT_EQ(hci::kHardwareFailure, status.protocol_error());
  });

  EXPECT_FALSE(discovery_manager()->discovering());

  RunLoopUntilIdle();

  EXPECT_FALSE(discovery_manager()->discovering());
}

// Test: When inquiry complete indicates failure, we signal to the current
// sessions.
TEST_F(BrEdrDiscoveryManagerTest, ContinuingDiscoveryError) {
  EXPECT_CMD_PACKET_OUT(test_device(), kInquiry, &kInquiryRsp, &kInquiryResult);
  EXPECT_CMD_PACKET_OUT(test_device(), kRemoteNameRequest1, &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete1);

  std::unique_ptr<BrEdrDiscoverySession> session;
  size_t peers_found = 0u;
  bool error_callback = false;

  discovery_manager()->RequestDiscovery(
      [&session, &peers_found, &error_callback](auto status, auto cb_session) {
        EXPECT_TRUE(status);
        cb_session->set_result_callback([&peers_found](const auto&) { peers_found++; });
        cb_session->set_error_callback([&error_callback]() { error_callback = true; });
        session = std::move(cb_session);
      });

  EXPECT_FALSE(discovery_manager()->discovering());

  RunLoopUntilIdle();

  EXPECT_EQ(1u, peers_found);
  EXPECT_TRUE(discovery_manager()->discovering());

  test_device()->SendCommandChannelPacket(kInquiryCompleteError);

  RunLoopUntilIdle();

  EXPECT_TRUE(error_callback);
  EXPECT_FALSE(discovery_manager()->discovering());

  session = nullptr;

  RunLoopUntilIdle();
}

// clang-format off
const auto kWriteLocalNameMaxLen = CreateStaticByteBuffer(
  LowerBits(hci::kWriteLocalName), UpperBits(hci::kWriteLocalName),
  0xF8, // parameter_total_size (248 bytes)
  // Complete Local Name (exactly 248 bytes)
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W'
);

const auto kWriteExtInquiryResponseMaxLen = CreateStaticByteBuffer(
  LowerBits(hci::kWriteExtendedInquiryResponse),
  UpperBits(hci::kWriteExtendedInquiryResponse),
  0xF1, // parameter_total_size (241 bytes)
  0x00, // fec_required
  0xEF, // 239 bytes (1 + 238 bytes)
  0x08, // DataType::kShortenedLocalName,
  // Shortened Local Name (238 bytes, truncated from above)
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M'
);
// clang-format on

// Test: UpdateLocalName successfully sends hci command, and further calls
// UpdateEIRResponseData (private). Ensures the name is updated at the very end.
TEST_F(BrEdrDiscoveryManagerTest, UpdateLocalNameShortenedSuccess) {
  EXPECT_CMD_PACKET_OUT(test_device(), kWriteLocalNameMaxLen, );

  // Set the status to be a dummy invalid status.
  hci::Status result = hci::Status(hci::kPairingNotAllowed);
  size_t callback_count = 0u;
  auto name_cb = [&result, &callback_count](const auto& status) {
    EXPECT_TRUE(status);
    callback_count++;
    result = status;
  };
  std::string kNewName = "";
  while (kNewName.length() < 225) {
    kNewName.append("ABCDEFGHIJKLMNOPQRSTUVWXY");
  }
  kNewName.append("ABCDEFGHIJKLMNOPQRSTUVW");
  discovery_manager()->UpdateLocalName(kNewName, name_cb);

  RunLoopUntilIdle();

  // Local name should not be set, callback shouldn't be called yet.
  EXPECT_NE(kNewName, discovery_manager()->local_name());
  EXPECT_EQ(0u, callback_count);

  test_device()->SendCommandChannelPacket(kWriteLocalNameRsp);
  EXPECT_CMD_PACKET_OUT(test_device(), kWriteExtInquiryResponseMaxLen, );

  RunLoopUntilIdle();

  // Still waiting on EIR response.
  // Local name should not be set, callback shouldn't be called yet.
  EXPECT_NE(kNewName, discovery_manager()->local_name());
  EXPECT_EQ(0u, callback_count);

  test_device()->SendCommandChannelPacket(kWriteExtendedInquiryResponseRsp);

  RunLoopUntilIdle();

  EXPECT_EQ(kNewName, discovery_manager()->local_name());
  EXPECT_EQ(hci::Status(hci::kSuccess), result);
  EXPECT_EQ(1u, callback_count);
}

// Test: UpdateLocalName successfully sends hci command, and further calls
// UpdateEIRResponseData (private). Ensures the name is updated at the very end.
TEST_F(BrEdrDiscoveryManagerTest, UpdateLocalNameSuccess) {
  EXPECT_CMD_PACKET_OUT(test_device(), kWriteLocalName, );

  // Set the status to be a dummy invalid status.
  hci::Status result = hci::Status(hci::kPairingNotAllowed);
  size_t callback_count = 0u;
  auto name_cb = [&result, &callback_count](const auto& status) {
    EXPECT_TRUE(status);
    callback_count++;
    result = status;
  };
  const std::string kNewName = "ABC";
  discovery_manager()->UpdateLocalName(kNewName, name_cb);

  RunLoopUntilIdle();

  // Local name should not be set, callback shouldn't be called yet.
  EXPECT_NE(kNewName, discovery_manager()->local_name());
  EXPECT_EQ(0u, callback_count);

  test_device()->SendCommandChannelPacket(kWriteLocalNameRsp);
  EXPECT_CMD_PACKET_OUT(test_device(), kWriteExtendedInquiryResponse, );

  RunLoopUntilIdle();

  // Still waiting on EIR response.
  // Local name should not be set, callback shouldn't be called yet.
  EXPECT_NE(kNewName, discovery_manager()->local_name());
  EXPECT_EQ(0u, callback_count);

  test_device()->SendCommandChannelPacket(kWriteExtendedInquiryResponseRsp);

  RunLoopUntilIdle();

  EXPECT_EQ(kNewName, discovery_manager()->local_name());
  EXPECT_EQ(hci::Status(hci::kSuccess), result);
  EXPECT_EQ(1u, callback_count);
}

// Test: UpdateLocalName passes back error code through the callback and |local_name_|
// does not get updated.
TEST_F(BrEdrDiscoveryManagerTest, UpdateLocalNameError) {
  EXPECT_CMD_PACKET_OUT(test_device(), kWriteLocalName, );

  // Set the status to be a dummy invalid status.
  hci::Status result = hci::Status(hci::kUnsupportedRemoteFeature);
  size_t callback_count = 0u;
  auto name_cb = [&result, &callback_count](const auto& status) {
    EXPECT_FALSE(status);
    callback_count++;
    result = status;
  };
  const std::string kNewName = "ABC";
  discovery_manager()->UpdateLocalName(kNewName, name_cb);

  RunLoopUntilIdle();

  // Local name should not be set, callback shouldn't be called yet.
  EXPECT_NE(kNewName, discovery_manager()->local_name());
  EXPECT_EQ(0u, callback_count);

  // Send a response error.
  test_device()->SendCommandChannelPacket(kWriteLocalNameRspError);

  RunLoopUntilIdle();

  // |local_name_| should not be updated, return status should be error.
  EXPECT_NE(kNewName, discovery_manager()->local_name());
  EXPECT_EQ(hci::Status(hci::kHardwareFailure), result);
  EXPECT_EQ(1u, callback_count);
}

// Test: UpdateLocalName should succeed, but UpdateEIRResponseData should fail.
// Consequently, the |local_name_| should not be updated, and the callback should
// return the error.
TEST_F(BrEdrDiscoveryManagerTest, UpdateEIRResponseDataError) {
  EXPECT_CMD_PACKET_OUT(test_device(), kWriteLocalName, );

  // Set the status to be a dummy invalid status.
  hci::Status result = hci::Status(hci::kUnsupportedRemoteFeature);
  size_t callback_count = 0u;
  auto name_cb = [&result, &callback_count](const auto& status) {
    EXPECT_FALSE(status);
    callback_count++;
    result = status;
  };
  const std::string kNewName = "ABC";
  discovery_manager()->UpdateLocalName(kNewName, name_cb);

  RunLoopUntilIdle();

  // Local name should not be set, callback shouldn't be called yet.
  EXPECT_NE(kNewName, discovery_manager()->local_name());
  EXPECT_EQ(0u, callback_count);

  // kWriteLocalName should succeed.
  test_device()->SendCommandChannelPacket(kWriteLocalNameRsp);
  EXPECT_CMD_PACKET_OUT(test_device(), kWriteExtendedInquiryResponse, );

  RunLoopUntilIdle();

  // Still waiting on EIR response.
  // Local name should not be set, callback shouldn't be called yet.
  EXPECT_NE(kNewName, discovery_manager()->local_name());
  EXPECT_EQ(0u, callback_count);

  // kWriteExtendedInquiryResponse should fail.
  test_device()->SendCommandChannelPacket(kWriteExtendedInquiryResponseRspError);

  RunLoopUntilIdle();

  // |local_name_| should not be updated, return status should be error.
  EXPECT_NE(kNewName, discovery_manager()->local_name());
  EXPECT_EQ(hci::Status(hci::kHardwareFailure), result);
  EXPECT_EQ(1u, callback_count);
}

// Test: requesting discoverable works
// Test: requesting discoverable while discoverable is pending doesn't send
// any more HCI commands
TEST_F(BrEdrDiscoveryManagerTest, DiscoverableSet) {
  EXPECT_CMD_PACKET_OUT(test_device(), kReadScanEnable, );

  std::vector<std::unique_ptr<BrEdrDiscoverableSession>> sessions;
  auto session_cb = [&sessions](auto status, auto cb_session) {
    EXPECT_TRUE(status);
    sessions.emplace_back(std::move(cb_session));
  };

  discovery_manager()->RequestDiscoverable(session_cb);

  RunLoopUntilIdle();

  EXPECT_EQ(0u, sessions.size());
  EXPECT_FALSE(discovery_manager()->discoverable());

  EXPECT_CMD_PACKET_OUT(test_device(), kWriteScanEnableInq, );

  test_device()->SendCommandChannelPacket(kReadScanEnableRspNone);

  RunLoopUntilIdle();

  // Request another session while the first is pending.
  discovery_manager()->RequestDiscoverable(session_cb);

  test_device()->SendCommandChannelPacket(kWriteScanEnableRsp);

  RunLoopUntilIdle();

  EXPECT_EQ(2u, sessions.size());
  EXPECT_TRUE(discovery_manager()->discoverable());

  discovery_manager()->RequestDiscoverable(session_cb);

  EXPECT_EQ(3u, sessions.size());
  EXPECT_TRUE(discovery_manager()->discoverable());

  EXPECT_CMD_PACKET_OUT(test_device(), kReadScanEnable, &kReadScanEnableRspInquiry);
  EXPECT_CMD_PACKET_OUT(test_device(), kWriteScanEnableNone, &kWriteScanEnableRsp);

  sessions.clear();

  RunLoopUntilIdle();

  EXPECT_FALSE(discovery_manager()->discoverable());
}

// Test: requesting discoverable while discovery is disabling leaves
// the discoverable enabled and reports success
// Test: enable/disable while page scan is enabled works.
TEST_F(BrEdrDiscoveryManagerTest, DiscoverableRequestWhileStopping) {
  EXPECT_CMD_PACKET_OUT(test_device(), kReadScanEnable, &kReadScanEnableRspPage);
  EXPECT_CMD_PACKET_OUT(test_device(), kWriteScanEnableBoth, &kWriteScanEnableRsp);

  std::vector<std::unique_ptr<BrEdrDiscoverableSession>> sessions;
  auto session_cb = [&sessions](auto status, auto cb_session) {
    EXPECT_TRUE(status);
    sessions.emplace_back(std::move(cb_session));
  };

  discovery_manager()->RequestDiscoverable(session_cb);

  RunLoopUntilIdle();

  EXPECT_EQ(1u, sessions.size());
  EXPECT_TRUE(discovery_manager()->discoverable());

  EXPECT_CMD_PACKET_OUT(test_device(), kReadScanEnable, );

  sessions.clear();

  RunLoopUntilIdle();

  // Request a new discovery before the procedure finishes.
  // This will queue another ReadScanEnable just in case the disable write is
  // in progress.
  EXPECT_CMD_PACKET_OUT(test_device(), kReadScanEnable, );
  discovery_manager()->RequestDiscoverable(session_cb);

  test_device()->SendCommandChannelPacket(kReadScanEnableRspBoth);

  // This shouldn't send any WriteScanEnable because we're already in the right
  // mode (MockController will assert if we do as it's not expecting)
  RunLoopUntilIdle();

  EXPECT_EQ(1u, sessions.size());
  EXPECT_TRUE(discovery_manager()->discoverable());

  // If somehow the scan got turned off, we will still turn it back on.
  EXPECT_CMD_PACKET_OUT(test_device(), kWriteScanEnableBoth, &kWriteScanEnableRsp);
  test_device()->SendCommandChannelPacket(kReadScanEnableRspPage);

  RunLoopUntilIdle();

  EXPECT_EQ(1u, sessions.size());
  EXPECT_TRUE(discovery_manager()->discoverable());

  EXPECT_CMD_PACKET_OUT(test_device(), kReadScanEnable, &kReadScanEnableRspBoth);
  EXPECT_CMD_PACKET_OUT(test_device(), kWriteScanEnablePage, &kWriteScanEnableRsp);

  sessions.clear();

  RunLoopUntilIdle();

  EXPECT_FALSE(discovery_manager()->discoverable());
}

// Test: non-standard inquiry modes mean before the first discovery, the
// inquiry mode is set.
// Test: extended inquiry is stored in the remote peer
TEST_F(BrEdrDiscoveryManagerTest, ExtendedInquiry) {
  NewDiscoveryManager(hci::InquiryMode::kExtended);

  EXPECT_CMD_PACKET_OUT(test_device(), kSetExtendedMode, &kSetExtendedModeRsp);
  EXPECT_CMD_PACKET_OUT(test_device(), kInquiry, &kInquiryRsp, &kExtendedInquiryResult,
                        &kRSSIInquiryResult);
  EXPECT_CMD_PACKET_OUT(test_device(), kRemoteNameRequest2, &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete2);

  std::unique_ptr<BrEdrDiscoverySession> session1;
  size_t peers_found1 = 0u;

  discovery_manager()->RequestDiscovery([&session1, &peers_found1](auto status, auto cb_session) {
    EXPECT_TRUE(status);
    cb_session->set_result_callback([&peers_found1](const auto&) { peers_found1++; });
    session1 = std::move(cb_session);
  });

  EXPECT_FALSE(discovery_manager()->discovering());

  RunLoopUntilIdle();

  EXPECT_TRUE(session1);
  EXPECT_EQ(2u, peers_found1);
  EXPECT_TRUE(discovery_manager()->discovering());
  session1 = nullptr;

  Peer* peer1 = peer_cache()->FindByAddress(kDeviceAddress2);
  ASSERT_TRUE(peer1);
  EXPECT_EQ(-20, peer1->rssi());

  Peer* peer2 = peer_cache()->FindByAddress(kDeviceAddress3);
  ASSERT_TRUE(peer2);
  ASSERT_TRUE(peer2->name());
  EXPECT_EQ("Fuchsia💖", *peer2->name());

  test_device()->SendCommandChannelPacket(kInquiryComplete);

  RunLoopUntilIdle();

  EXPECT_FALSE(discovery_manager()->discovering());
}

TEST_F(BrEdrDiscoveryManagerTest, InquiryResultUpgradesKnownLowEnergyPeerToDualMode) {
  Peer* peer = peer_cache()->NewPeer(kLeAliasAddress1, true);
  ASSERT_TRUE(peer);
  ASSERT_EQ(TechnologyType::kLowEnergy, peer->technology());

  EXPECT_CMD_PACKET_OUT(test_device(), kInquiry, &kInquiryRsp, &kInquiryResult);
  EXPECT_CMD_PACKET_OUT(test_device(), kRemoteNameRequest1, &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete1);

  std::unique_ptr<BrEdrDiscoverySession> session;
  size_t peers_found = 0u;

  discovery_manager()->RequestDiscovery([&session, &peers_found](auto status, auto cb_session) {
    EXPECT_TRUE(status);
    cb_session->set_result_callback([&peers_found](auto&) { peers_found++; });
    session = std::move(cb_session);
  });
  RunLoopUntilIdle();
  session = nullptr;

  EXPECT_EQ(1u, peers_found);
  ASSERT_EQ(peer, peer_cache()->FindByAddress(kDeviceAddress1));
  EXPECT_EQ(TechnologyType::kDualMode, peer->technology());

  test_device()->SendCommandChannelPacket(kInquiryComplete);

  RunLoopUntilIdle();
}

TEST_F(BrEdrDiscoveryManagerTest, ExtendedInquiryResultUpgradesKnownLowEnergyPeerToDualMode) {
  Peer* peer = peer_cache()->NewPeer(kLeAliasAddress2, true);
  ASSERT_TRUE(peer);
  ASSERT_EQ(TechnologyType::kLowEnergy, peer->technology());

  NewDiscoveryManager(hci::InquiryMode::kExtended);

  EXPECT_CMD_PACKET_OUT(test_device(), kSetExtendedMode, &kSetExtendedModeRsp);
  EXPECT_CMD_PACKET_OUT(test_device(), kInquiry, &kInquiryRsp, &kExtendedInquiryResult,
                        &kRSSIInquiryResult);
  EXPECT_CMD_PACKET_OUT(test_device(), kRemoteNameRequest2, &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete2);

  std::unique_ptr<BrEdrDiscoverySession> session;
  size_t peers_found = 0u;

  discovery_manager()->RequestDiscovery([&session, &peers_found](auto status, auto cb_session) {
    EXPECT_TRUE(status);
    cb_session->set_result_callback([&peers_found](auto&) { peers_found++; });
    session = std::move(cb_session);
  });
  RunLoopUntilIdle();
  session = nullptr;

  EXPECT_EQ(2u, peers_found);
  ASSERT_EQ(peer, peer_cache()->FindByAddress(kDeviceAddress2));
  EXPECT_EQ(TechnologyType::kDualMode, peer->technology());

  test_device()->SendCommandChannelPacket(kInquiryComplete);

  RunLoopUntilIdle();
}

}  // namespace
}  // namespace bt::gap

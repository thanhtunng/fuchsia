// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sdmmc-block-device.h"

#include <endian.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fake_ddk/fake_ddk.h>
#include <lib/fake_ddk/fidl-helper.h>
#include <lib/fidl/llcpp/client.h>
#include <lib/fit/defer.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/sdmmc/hw.h>

#include <fbl/algorithm.h>
#include <zxtest/zxtest.h>

#include "fake-sdmmc-device.h"

namespace sdmmc {

class SdmmcBlockDeviceTest : public zxtest::Test {
 public:
  SdmmcBlockDeviceTest()
      : dut_(fake_ddk::kFakeParent, SdmmcDevice(sdmmc_.GetClient())),
        loop_(&kAsyncLoopConfigAttachToCurrentThread) {
    dut_.SetBlockInfo(FakeSdmmcDevice::kBlockSize, FakeSdmmcDevice::kBlockCount);
    for (size_t i = 0; i < (FakeSdmmcDevice::kBlockSize / sizeof(kTestData)); i++) {
      test_block_.insert(test_block_.end(), kTestData, kTestData + sizeof(kTestData));
    }
  }

  void SetUp() override {
    sdmmc_.Reset();

    sdmmc_.set_command_callback(SDMMC_SEND_CSD, [](sdmmc_req_t* req) -> void {
      uint8_t* response = reinterpret_cast<uint8_t*>(req->response);
      response[MMC_CSD_SPEC_VERSION] = MMC_CID_SPEC_VRSN_40 << 2;
      response[MMC_CSD_SIZE_START] = 0x03 << 6;
      response[MMC_CSD_SIZE_START + 1] = 0xff;
      response[MMC_CSD_SIZE_START + 2] = 0x03;
    });

    sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) -> void {
      uint8_t* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer) + req->buf_offset;
      *reinterpret_cast<uint32_t*>(&ext_csd[212]) = htole32(kBlockCount);
      ext_csd[MMC_EXT_CSD_PARTITION_CONFIG] = 0xa8;
      ext_csd[MMC_EXT_CSD_EXT_CSD_REV] = 6;
      ext_csd[MMC_EXT_CSD_PARTITION_SWITCH_TIME] = 0;
      ext_csd[MMC_EXT_CSD_BOOT_SIZE_MULT] = 0x10;
      ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 0;
    });
  }

  void TearDown() override { dut_.StopWorkerThread(); }

  void QueueBlockOps();
  void QueueRpmbRequests();

 protected:
  static constexpr uint32_t kBlockCount = 0x100000;
  static constexpr size_t kBlockOpSize = BlockOperation::OperationSize(sizeof(block_op_t));
  static constexpr uint32_t kMaxOutstandingOps = 16;

  struct OperationContext {
    zx::vmo vmo;
    fzl::VmoMapper mapper;
    zx_status_t status;
    bool completed;
  };

  struct CallbackContext {
    CallbackContext(uint32_t exp_op) : expected_operations(exp_op) {}
    uint32_t expected_operations;
    sync_completion_t completion;
  };

  static void OperationCallback(void* ctx, zx_status_t status, block_op_t* op) {
    auto* const cb_ctx = reinterpret_cast<CallbackContext*>(ctx);

    block::Operation<OperationContext> block_op(op, kBlockOpSize, false);
    block_op.private_storage()->completed = true;
    block_op.private_storage()->status = status;

    if (--(cb_ctx->expected_operations) == 0) {
      sync_completion_signal(&cb_ctx->completion);
    }
  }

  void AddDevice() {
    EXPECT_OK(dut_.ProbeMmc());

    EXPECT_OK(dut_.AddDevice());

    user_ = GetBlockClient(USER_DATA_PARTITION);
    boot1_ = GetBlockClient(BOOT_PARTITION_1);
    boot2_ = GetBlockClient(BOOT_PARTITION_2);
    rpmb_ = GetRpmbClient(RPMB_PARTITION);

    ASSERT_TRUE(user_.is_valid());

    const auto message_op = [](void* ctx, fidl_incoming_msg_t* msg,
                               fidl_txn_t* txn) -> zx_status_t {
      return static_cast<decltype(ddk_)*>(ctx)->MessageChild(RPMB_PARTITION, msg, txn);
    };
    ASSERT_OK(messenger_.SetMessageOp(&ddk_, message_op));
    ASSERT_OK(loop_.StartThread("rpmb-client-thread"));
    rpmb_fidl_.Bind(std::move(messenger_.local()), loop_.dispatcher());
  }

  void MakeBlockOp(uint32_t command, uint32_t length, uint64_t offset,
                   std::optional<block::Operation<OperationContext>>* out_op) {
    *out_op = block::Operation<OperationContext>::Alloc(kBlockOpSize);
    ASSERT_TRUE(*out_op);

    if (command == BLOCK_OP_READ || command == BLOCK_OP_WRITE) {
      (*out_op)->operation()->rw = {
          .command = command,
          .extra = 0,
          .vmo = ZX_HANDLE_INVALID,
          .length = length,
          .offset_dev = offset,
          .offset_vmo = 0,
      };

      if (length > 0) {
        OperationContext* const ctx = (*out_op)->private_storage();
        const size_t vmo_size = fbl::round_up<size_t, size_t>(length * FakeSdmmcDevice::kBlockSize,
                                                              zx_system_get_page_size());
        ASSERT_OK(ctx->mapper.CreateAndMap(vmo_size, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, nullptr,
                                           &ctx->vmo));
        ctx->completed = false;
        ctx->status = ZX_OK;
        (*out_op)->operation()->rw.vmo = ctx->vmo.get();
      }
    } else if (command == BLOCK_OP_TRIM) {
      (*out_op)->operation()->trim = {
          .command = command,
          .length = length,
          .offset_dev = offset,
      };
    } else {
      (*out_op)->operation()->command = command;
    }
  }

  void FillSdmmc(uint32_t length, uint64_t offset) {
    for (uint32_t i = 0; i < length; i++) {
      sdmmc_.Write((offset + i) * test_block_.size(), test_block_);
    }
  }

  void FillVmo(const fzl::VmoMapper& mapper, uint32_t length, uint64_t offset = 0) {
    auto* ptr = reinterpret_cast<uint8_t*>(mapper.start()) + (offset * test_block_.size());
    for (uint32_t i = 0; i < length; i++, ptr += test_block_.size()) {
      memcpy(ptr, test_block_.data(), test_block_.size());
    }
  }

  void CheckSdmmc(uint32_t length, uint64_t offset) {
    const std::vector<uint8_t> data =
        sdmmc_.Read(offset * test_block_.size(), length * test_block_.size());
    const uint8_t* ptr = data.data();
    for (uint32_t i = 0; i < length; i++, ptr += test_block_.size()) {
      EXPECT_BYTES_EQ(ptr, test_block_.data(), test_block_.size());
    }
  }

  void CheckVmo(const fzl::VmoMapper& mapper, uint32_t length, uint64_t offset = 0) {
    const uint8_t* ptr = reinterpret_cast<uint8_t*>(mapper.start()) + (offset * test_block_.size());
    for (uint32_t i = 0; i < length; i++, ptr += test_block_.size()) {
      EXPECT_BYTES_EQ(ptr, test_block_.data(), test_block_.size());
    }
  }

  void CheckVmoErased(const fzl::VmoMapper& mapper, uint32_t length, uint64_t offset = 0) {
    const size_t blocks_to_u32 = test_block_.size() / sizeof(uint32_t);
    const uint32_t* data = reinterpret_cast<uint32_t*>(mapper.start()) + (offset * blocks_to_u32);
    for (uint32_t i = 0; i < (length * blocks_to_u32); i++) {
      EXPECT_EQ(data[i], 0xffff'ffff);
    }
  }

  ddk::BlockImplProtocolClient GetBlockClient(size_t index) {
    block_impl_protocol_t proto;
    if (ddk_.GetChildProtocol(index, ZX_PROTOCOL_BLOCK_IMPL, &proto) != ZX_OK) {
      return ddk::BlockImplProtocolClient();
    }
    return ddk::BlockImplProtocolClient(&proto);
  }

  ddk::RpmbProtocolClient GetRpmbClient(size_t index) {
    rpmb_protocol_t proto;
    if (ddk_.GetChildProtocol(index, &proto) != ZX_OK) {
      return ddk::RpmbProtocolClient();
    }
    return ddk::RpmbProtocolClient(&proto);
  }

  FakeSdmmcDevice sdmmc_;
  SdmmcBlockDevice dut_;
  ddk::BlockImplProtocolClient user_;
  ddk::BlockImplProtocolClient boot1_;
  ddk::BlockImplProtocolClient boot2_;
  ddk::RpmbProtocolClient rpmb_;
  fidl::WireSharedClient<fuchsia_hardware_rpmb::Rpmb> rpmb_fidl_;
  Bind ddk_;
  std::atomic<bool> run_threads_ = true;
  async::Loop loop_;

 private:
  static constexpr uint8_t kTestData[] = {
      // clang-format off
      0xd0, 0x0d, 0x7a, 0xf2, 0xbc, 0x13, 0x81, 0x07,
      0x72, 0xbe, 0x33, 0x5f, 0x21, 0x4e, 0xd7, 0xba,
      0x1b, 0x0c, 0x25, 0xcf, 0x2c, 0x6f, 0x46, 0x3a,
      0x78, 0x22, 0xea, 0x9e, 0xa0, 0x41, 0x65, 0xf8,
      // clang-format on
  };
  static_assert(FakeSdmmcDevice::kBlockSize % sizeof(kTestData) == 0);

  std::vector<uint8_t> test_block_;
  fake_ddk::FidlMessenger messenger_;
};

TEST_F(SdmmcBlockDeviceTest, BlockImplQuery) {
  AddDevice();

  size_t block_op_size;
  block_info_t info;
  user_.Query(&info, &block_op_size);

  EXPECT_EQ(info.block_count, kBlockCount);
  EXPECT_EQ(info.block_size, FakeSdmmcDevice::kBlockSize);
  EXPECT_EQ(block_op_size, kBlockOpSize);
}

TEST_F(SdmmcBlockDeviceTest, BlockImplQueue) {
  AddDevice();

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, 0, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 5, 0x8000, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_FLUSH, 0, 0, &op3));

  std::optional<block::Operation<OperationContext>> op4;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 1, 0x400, &op4));

  std::optional<block::Operation<OperationContext>> op5;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 10, 0x2000, &op5));

  CallbackContext ctx(5);

  FillVmo(op1->private_storage()->mapper, 1);
  FillVmo(op2->private_storage()->mapper, 5);
  FillSdmmc(1, 0x400);
  FillSdmmc(10, 0x2000);

  user_.Queue(op1->operation(), OperationCallback, &ctx);
  user_.Queue(op2->operation(), OperationCallback, &ctx);
  user_.Queue(op3->operation(), OperationCallback, &ctx);
  user_.Queue(op4->operation(), OperationCallback, &ctx);
  user_.Queue(op5->operation(), OperationCallback, &ctx);

  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  EXPECT_TRUE(op1->private_storage()->completed);
  EXPECT_TRUE(op2->private_storage()->completed);
  EXPECT_TRUE(op3->private_storage()->completed);
  EXPECT_TRUE(op4->private_storage()->completed);
  EXPECT_TRUE(op5->private_storage()->completed);

  EXPECT_OK(op1->private_storage()->status);
  EXPECT_OK(op2->private_storage()->status);
  EXPECT_OK(op3->private_storage()->status);
  EXPECT_OK(op4->private_storage()->status);
  EXPECT_OK(op5->private_storage()->status);

  ASSERT_NO_FATAL_FAILURES(CheckSdmmc(1, 0));
  ASSERT_NO_FATAL_FAILURES(CheckSdmmc(5, 0x8000));
  ASSERT_NO_FATAL_FAILURES(CheckVmo(op4->private_storage()->mapper, 1));
  ASSERT_NO_FATAL_FAILURES(CheckVmo(op5->private_storage()->mapper, 10));
}

TEST_F(SdmmcBlockDeviceTest, BlockImplQueueOutOfRange) {
  AddDevice();

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, 0x100000, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 10, 0x200000, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 8, 0xffff8, &op3));

  std::optional<block::Operation<OperationContext>> op4;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 9, 0xffff8, &op4));

  std::optional<block::Operation<OperationContext>> op5;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 16, 0xffff8, &op5));

  std::optional<block::Operation<OperationContext>> op6;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 0, 0x80000, &op6));

  std::optional<block::Operation<OperationContext>> op7;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, 0xfffff, &op7));

  CallbackContext ctx(7);

  user_.Queue(op1->operation(), OperationCallback, &ctx);
  user_.Queue(op2->operation(), OperationCallback, &ctx);
  user_.Queue(op3->operation(), OperationCallback, &ctx);
  user_.Queue(op4->operation(), OperationCallback, &ctx);
  user_.Queue(op5->operation(), OperationCallback, &ctx);
  user_.Queue(op6->operation(), OperationCallback, &ctx);
  user_.Queue(op7->operation(), OperationCallback, &ctx);

  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  EXPECT_TRUE(op1->private_storage()->completed);
  EXPECT_TRUE(op2->private_storage()->completed);
  EXPECT_TRUE(op3->private_storage()->completed);
  EXPECT_TRUE(op4->private_storage()->completed);
  EXPECT_TRUE(op5->private_storage()->completed);
  EXPECT_TRUE(op6->private_storage()->completed);
  EXPECT_TRUE(op7->private_storage()->completed);

  EXPECT_NOT_OK(op1->private_storage()->status);
  EXPECT_NOT_OK(op2->private_storage()->status);
  EXPECT_OK(op3->private_storage()->status);
  EXPECT_NOT_OK(op4->private_storage()->status);
  EXPECT_NOT_OK(op5->private_storage()->status);
  EXPECT_OK(op6->private_storage()->status);
  EXPECT_OK(op7->private_storage()->status);
}

TEST_F(SdmmcBlockDeviceTest, MultiBlockACmd12) {
  AddDevice();

  sdmmc_.set_host_info({
      .caps = SDMMC_HOST_CAP_AUTO_CMD12,
      .max_transfer_size = BLOCK_MAX_TRANSFER_UNBOUNDED,
      .max_transfer_size_non_dma = 0,
      .prefs = 0,
  });
  EXPECT_OK(dut_.Init());

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, 0, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 5, 0x8000, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_FLUSH, 0, 0, &op3));

  std::optional<block::Operation<OperationContext>> op4;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 1, 0x400, &op4));

  std::optional<block::Operation<OperationContext>> op5;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 10, 0x2000, &op5));

  CallbackContext ctx(5);

  sdmmc_.set_command_callback(SDMMC_READ_MULTIPLE_BLOCK, [](sdmmc_req_t* req) -> void {
    EXPECT_TRUE(req->cmd_flags & SDMMC_CMD_AUTO12);
  });
  sdmmc_.set_command_callback(SDMMC_WRITE_MULTIPLE_BLOCK, [](sdmmc_req_t* req) -> void {
    EXPECT_TRUE(req->cmd_flags & SDMMC_CMD_AUTO12);
  });

  user_.Queue(op1->operation(), OperationCallback, &ctx);
  user_.Queue(op2->operation(), OperationCallback, &ctx);
  user_.Queue(op3->operation(), OperationCallback, &ctx);
  user_.Queue(op4->operation(), OperationCallback, &ctx);
  user_.Queue(op5->operation(), OperationCallback, &ctx);

  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  const std::map<uint32_t, uint32_t> command_counts = sdmmc_.command_counts();
  EXPECT_EQ(command_counts.find(SDMMC_STOP_TRANSMISSION), command_counts.end());
}

TEST_F(SdmmcBlockDeviceTest, MultiBlockNoACmd12) {
  AddDevice();

  sdmmc_.set_host_info({
      .caps = 0,
      .max_transfer_size = BLOCK_MAX_TRANSFER_UNBOUNDED,
      .max_transfer_size_non_dma = 0,
      .prefs = 0,
  });
  EXPECT_OK(dut_.Init());

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, 0, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 5, 0x8000, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_FLUSH, 0, 0, &op3));

  std::optional<block::Operation<OperationContext>> op4;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 1, 0x400, &op4));

  std::optional<block::Operation<OperationContext>> op5;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 10, 0x2000, &op5));

  CallbackContext ctx(5);

  sdmmc_.set_command_callback(SDMMC_READ_MULTIPLE_BLOCK, [](sdmmc_req_t* req) -> void {
    EXPECT_FALSE(req->cmd_flags & SDMMC_CMD_AUTO12);
  });
  sdmmc_.set_command_callback(SDMMC_WRITE_MULTIPLE_BLOCK, [](sdmmc_req_t* req) -> void {
    EXPECT_FALSE(req->cmd_flags & SDMMC_CMD_AUTO12);
  });

  user_.Queue(op1->operation(), OperationCallback, &ctx);
  user_.Queue(op2->operation(), OperationCallback, &ctx);
  user_.Queue(op3->operation(), OperationCallback, &ctx);
  user_.Queue(op4->operation(), OperationCallback, &ctx);
  user_.Queue(op5->operation(), OperationCallback, &ctx);

  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  EXPECT_EQ(sdmmc_.command_counts().at(SDMMC_STOP_TRANSMISSION), 2);
}

TEST_F(SdmmcBlockDeviceTest, ErrorsPropagate) {
  AddDevice();

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, FakeSdmmcDevice::kBadRegionStart, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(
      MakeBlockOp(BLOCK_OP_WRITE, 5, FakeSdmmcDevice::kBadRegionStart | 0x80, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_FLUSH, 0, 0, &op3));

  std::optional<block::Operation<OperationContext>> op4;
  ASSERT_NO_FATAL_FAILURES(
      MakeBlockOp(BLOCK_OP_READ, 1, FakeSdmmcDevice::kBadRegionStart | 0x40, &op4));

  std::optional<block::Operation<OperationContext>> op5;
  ASSERT_NO_FATAL_FAILURES(
      MakeBlockOp(BLOCK_OP_READ, 10, FakeSdmmcDevice::kBadRegionStart | 0x20, &op5));

  CallbackContext ctx(5);

  user_.Queue(op1->operation(), OperationCallback, &ctx);
  user_.Queue(op2->operation(), OperationCallback, &ctx);
  user_.Queue(op3->operation(), OperationCallback, &ctx);
  user_.Queue(op4->operation(), OperationCallback, &ctx);
  user_.Queue(op5->operation(), OperationCallback, &ctx);

  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  EXPECT_TRUE(op1->private_storage()->completed);
  EXPECT_TRUE(op2->private_storage()->completed);
  EXPECT_TRUE(op3->private_storage()->completed);
  EXPECT_TRUE(op4->private_storage()->completed);
  EXPECT_TRUE(op5->private_storage()->completed);

  EXPECT_NOT_OK(op1->private_storage()->status);
  EXPECT_NOT_OK(op2->private_storage()->status);
  EXPECT_OK(op3->private_storage()->status);
  EXPECT_NOT_OK(op4->private_storage()->status);
  EXPECT_NOT_OK(op5->private_storage()->status);
}

TEST_F(SdmmcBlockDeviceTest, SendCmd12OnCommandFailure) {
  AddDevice();

  sdmmc_.set_host_info({
      .caps = 0,
      .max_transfer_size = BLOCK_MAX_TRANSFER_UNBOUNDED,
      .max_transfer_size_non_dma = 0,
      .prefs = 0,
  });
  EXPECT_OK(dut_.Init());

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, FakeSdmmcDevice::kBadRegionStart, &op1));
  CallbackContext ctx1(1);

  user_.Queue(op1->operation(), OperationCallback, &ctx1);

  EXPECT_OK(sync_completion_wait(&ctx1.completion, zx::duration::infinite().get()));
  EXPECT_TRUE(op1->private_storage()->completed);
  EXPECT_EQ(sdmmc_.command_counts().at(SDMMC_STOP_TRANSMISSION), 1);

  sdmmc_.set_host_info({
      .caps = SDMMC_HOST_CAP_AUTO_CMD12,
      .max_transfer_size = BLOCK_MAX_TRANSFER_UNBOUNDED,
      .max_transfer_size_non_dma = 0,
      .prefs = 0,
  });
  EXPECT_OK(dut_.Init());

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, FakeSdmmcDevice::kBadRegionStart, &op2));
  CallbackContext ctx2(1);

  user_.Queue(op2->operation(), OperationCallback, &ctx2);

  EXPECT_OK(sync_completion_wait(&ctx2.completion, zx::duration::infinite().get()));
  EXPECT_TRUE(op2->private_storage()->completed);
  EXPECT_EQ(sdmmc_.command_counts().at(SDMMC_STOP_TRANSMISSION), 2);
}

// TODO(fxbug.dev/49028): Enable these tests once trim is enabled.
TEST_F(SdmmcBlockDeviceTest, DISABLED_Trim) {
  AddDevice();

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 10, 100, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_FLUSH, 0, 0, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 10, 100, &op3));

  std::optional<block::Operation<OperationContext>> op4;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_TRIM, 1, 103, &op4));

  std::optional<block::Operation<OperationContext>> op5;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 10, 100, &op5));

  std::optional<block::Operation<OperationContext>> op6;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_TRIM, 3, 106, &op6));

  std::optional<block::Operation<OperationContext>> op7;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 10, 100, &op7));

  FillVmo(op1->private_storage()->mapper, 10);

  CallbackContext ctx(7);

  user_.Queue(op1->operation(), OperationCallback, &ctx);
  user_.Queue(op2->operation(), OperationCallback, &ctx);
  user_.Queue(op3->operation(), OperationCallback, &ctx);
  user_.Queue(op4->operation(), OperationCallback, &ctx);
  user_.Queue(op5->operation(), OperationCallback, &ctx);
  user_.Queue(op6->operation(), OperationCallback, &ctx);
  user_.Queue(op7->operation(), OperationCallback, &ctx);

  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  ASSERT_NO_FATAL_FAILURES(CheckVmo(op3->private_storage()->mapper, 10, 0));

  ASSERT_NO_FATAL_FAILURES(CheckVmo(op5->private_storage()->mapper, 3, 0));
  ASSERT_NO_FATAL_FAILURES(CheckVmoErased(op5->private_storage()->mapper, 1, 3));
  ASSERT_NO_FATAL_FAILURES(CheckVmo(op5->private_storage()->mapper, 6, 4));

  ASSERT_NO_FATAL_FAILURES(CheckVmo(op7->private_storage()->mapper, 3, 0));
  ASSERT_NO_FATAL_FAILURES(CheckVmoErased(op7->private_storage()->mapper, 1, 3));
  ASSERT_NO_FATAL_FAILURES(CheckVmo(op7->private_storage()->mapper, 2, 4));
  ASSERT_NO_FATAL_FAILURES(CheckVmoErased(op7->private_storage()->mapper, 3, 6));
  ASSERT_NO_FATAL_FAILURES(CheckVmo(op7->private_storage()->mapper, 1, 9));

  EXPECT_OK(op1->private_storage()->status);
  EXPECT_OK(op2->private_storage()->status);
  EXPECT_OK(op3->private_storage()->status);
  EXPECT_OK(op4->private_storage()->status);
  EXPECT_OK(op5->private_storage()->status);
  EXPECT_OK(op6->private_storage()->status);
  EXPECT_OK(op7->private_storage()->status);
}

TEST_F(SdmmcBlockDeviceTest, DISABLED_TrimErrors) {
  AddDevice();

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_TRIM, 10, 10, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(
      MakeBlockOp(BLOCK_OP_TRIM, 10, FakeSdmmcDevice::kBadRegionStart | 0x40, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(
      MakeBlockOp(BLOCK_OP_TRIM, 10, FakeSdmmcDevice::kBadRegionStart - 5, &op3));

  std::optional<block::Operation<OperationContext>> op4;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_TRIM, 10, 100, &op4));

  std::optional<block::Operation<OperationContext>> op5;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_TRIM, 10, 110, &op5));

  sdmmc_.set_command_callback(MMC_ERASE_GROUP_START, [](sdmmc_req_t* req) {
    if (req->arg == 100) {
      req->response[0] |= MMC_STATUS_ERASE_SEQ_ERR;
    }
  });

  sdmmc_.set_command_callback(MMC_ERASE_GROUP_END, [](sdmmc_req_t* req) {
    if (req->arg == 119) {
      req->response[0] |= MMC_STATUS_ADDR_OUT_OF_RANGE;
    }
  });

  CallbackContext ctx(5);

  user_.Queue(op1->operation(), OperationCallback, &ctx);
  user_.Queue(op2->operation(), OperationCallback, &ctx);
  user_.Queue(op3->operation(), OperationCallback, &ctx);
  user_.Queue(op4->operation(), OperationCallback, &ctx);
  user_.Queue(op5->operation(), OperationCallback, &ctx);

  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  EXPECT_OK(op1->private_storage()->status);
  EXPECT_NOT_OK(op2->private_storage()->status);
  EXPECT_NOT_OK(op3->private_storage()->status);
  EXPECT_NOT_OK(op4->private_storage()->status);
  EXPECT_NOT_OK(op5->private_storage()->status);
}

TEST_F(SdmmcBlockDeviceTest, DdkLifecycle) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    uint8_t* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    ext_csd[MMC_EXT_CSD_PARTITION_SWITCH_TIME] = 0;
    ext_csd[MMC_EXT_CSD_BOOT_SIZE_MULT] = 0;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 0;
  });

  AddDevice();

  dut_.DdkAsyncRemove();
  ASSERT_NO_FATAL_FAILURES(ddk_.Ok());
  EXPECT_EQ(ddk_.total_children(), 1);
}

TEST_F(SdmmcBlockDeviceTest, DdkLifecycleBootPartitionsExistButNotUsed) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    uint8_t* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    ext_csd[MMC_EXT_CSD_PARTITION_CONFIG] = 2;
    ext_csd[MMC_EXT_CSD_PARTITION_SWITCH_TIME] = 0;
    ext_csd[MMC_EXT_CSD_BOOT_SIZE_MULT] = 1;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 0;
  });

  AddDevice();

  dut_.DdkAsyncRemove();
  ASSERT_NO_FATAL_FAILURES(ddk_.Ok());
  EXPECT_EQ(ddk_.total_children(), 1);
}

TEST_F(SdmmcBlockDeviceTest, DdkLifecycleWithBootPartitions) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    uint8_t* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    ext_csd[MMC_EXT_CSD_PARTITION_CONFIG] = 0xa8;
    ext_csd[MMC_EXT_CSD_PARTITION_SWITCH_TIME] = 0;
    ext_csd[MMC_EXT_CSD_BOOT_SIZE_MULT] = 1;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 0;
  });

  AddDevice();

  dut_.DdkAsyncRemove();
  ASSERT_NO_FATAL_FAILURES(ddk_.Ok());
  EXPECT_EQ(ddk_.total_children(), 3);
}

TEST_F(SdmmcBlockDeviceTest, DdkLifecycleWithBootAndRpmbPartitions) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    uint8_t* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    ext_csd[MMC_EXT_CSD_RPMB_SIZE_MULT] = 1;
    ext_csd[MMC_EXT_CSD_PARTITION_CONFIG] = 0xa8;
    ext_csd[MMC_EXT_CSD_PARTITION_SWITCH_TIME] = 0;
    ext_csd[MMC_EXT_CSD_BOOT_SIZE_MULT] = 1;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 0;
  });

  AddDevice();

  dut_.DdkAsyncRemove();
  ASSERT_NO_FATAL_FAILURES(ddk_.Ok());
  EXPECT_EQ(ddk_.total_children(), 4);
}

TEST_F(SdmmcBlockDeviceTest, CompleteTransactions) {
  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, 0, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 5, 0x8000, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_FLUSH, 0, 0, &op3));

  std::optional<block::Operation<OperationContext>> op4;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 1, 0x400, &op4));

  std::optional<block::Operation<OperationContext>> op5;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 10, 0x2000, &op5));

  CallbackContext ctx(5);

  {
    SdmmcBlockDevice dut(fake_ddk::kFakeParent, SdmmcDevice(sdmmc_.GetClient()));
    dut.SetBlockInfo(FakeSdmmcDevice::kBlockSize, FakeSdmmcDevice::kBlockCount);
    EXPECT_OK(dut.AddDevice());

    auto stop_threads = fit::defer([&]() { dut.StopWorkerThread(); });

    ddk::BlockImplProtocolClient user = GetBlockClient(USER_DATA_PARTITION);
    ASSERT_TRUE(user.is_valid());

    user.Queue(op1->operation(), OperationCallback, &ctx);
    user.Queue(op2->operation(), OperationCallback, &ctx);
    user.Queue(op3->operation(), OperationCallback, &ctx);
    user.Queue(op4->operation(), OperationCallback, &ctx);
    user.Queue(op5->operation(), OperationCallback, &ctx);
  }

  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  EXPECT_TRUE(op1->private_storage()->completed);
  EXPECT_TRUE(op2->private_storage()->completed);
  EXPECT_TRUE(op3->private_storage()->completed);
  EXPECT_TRUE(op4->private_storage()->completed);
  EXPECT_TRUE(op5->private_storage()->completed);
}

TEST_F(SdmmcBlockDeviceTest, CompleteTransactionsOnUnbind) {
  AddDevice();
  dut_.StopWorkerThread();  // Stop the worker thread so queued requests don't get completed.

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, 0, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 5, 0x8000, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_FLUSH, 0, 0, &op3));

  std::optional<block::Operation<OperationContext>> op4;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 1, 0x400, &op4));

  std::optional<block::Operation<OperationContext>> op5;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 10, 0x2000, &op5));

  CallbackContext ctx(5);

  user_.Queue(op1->operation(), OperationCallback, &ctx);
  user_.Queue(op2->operation(), OperationCallback, &ctx);
  user_.Queue(op3->operation(), OperationCallback, &ctx);
  user_.Queue(op4->operation(), OperationCallback, &ctx);
  user_.Queue(op5->operation(), OperationCallback, &ctx);

  dut_.DdkUnbind(ddk::UnbindTxn(dut_.zxdev()));

  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  EXPECT_TRUE(op1->private_storage()->completed);
  EXPECT_TRUE(op2->private_storage()->completed);
  EXPECT_TRUE(op3->private_storage()->completed);
  EXPECT_TRUE(op4->private_storage()->completed);
  EXPECT_TRUE(op5->private_storage()->completed);
}

TEST_F(SdmmcBlockDeviceTest, CompleteTransactionsOnSuspend) {
  AddDevice();
  dut_.StopWorkerThread();

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, 0, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 5, 0x8000, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_FLUSH, 0, 0, &op3));

  std::optional<block::Operation<OperationContext>> op4;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 1, 0x400, &op4));

  std::optional<block::Operation<OperationContext>> op5;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 10, 0x2000, &op5));

  CallbackContext ctx(5);

  user_.Queue(op1->operation(), OperationCallback, &ctx);
  user_.Queue(op2->operation(), OperationCallback, &ctx);
  user_.Queue(op3->operation(), OperationCallback, &ctx);
  user_.Queue(op4->operation(), OperationCallback, &ctx);
  user_.Queue(op5->operation(), OperationCallback, &ctx);

  dut_.DdkSuspend(ddk::SuspendTxn(dut_.zxdev(), 0, false, 0));

  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  EXPECT_TRUE(op1->private_storage()->completed);
  EXPECT_TRUE(op2->private_storage()->completed);
  EXPECT_TRUE(op3->private_storage()->completed);
  EXPECT_TRUE(op4->private_storage()->completed);
  EXPECT_TRUE(op5->private_storage()->completed);
}

TEST_F(SdmmcBlockDeviceTest, ProbeMmcSendStatusRetry) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    uint8_t* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    ext_csd[MMC_EXT_CSD_DEVICE_TYPE] = 1 << 4;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 1;
  });
  sdmmc_.set_command_callback(SDMMC_SEND_STATUS, [](sdmmc_req_t* req) {
    // Fail twice before succeeding.
    static uint32_t call_count = 0;
    if (++call_count >= 3) {
      req->status = ZX_OK;
      call_count = 0;
    } else {
      req->status = ZX_ERR_IO_DATA_INTEGRITY;
    }
  });

  SdmmcBlockDevice dut(nullptr, SdmmcDevice(sdmmc_.GetClient()));
  EXPECT_OK(dut.ProbeMmc());
}

TEST_F(SdmmcBlockDeviceTest, ProbeMmcSendStatusFail) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    uint8_t* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    ext_csd[MMC_EXT_CSD_DEVICE_TYPE] = 1 << 4;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 1;
  });
  sdmmc_.set_command_callback(SDMMC_SEND_STATUS,
                              [](sdmmc_req_t* req) { req->status = ZX_ERR_IO_DATA_INTEGRITY; });

  SdmmcBlockDevice dut(nullptr, SdmmcDevice(sdmmc_.GetClient()));
  EXPECT_NOT_OK(dut.ProbeMmc());
}

TEST_F(SdmmcBlockDeviceTest, QueryBootPartitions) {
  AddDevice();

  ASSERT_TRUE(boot1_.is_valid());
  ASSERT_TRUE(boot2_.is_valid());

  size_t boot1_op_size, boot2_op_size;
  block_info_t boot1_info, boot2_info;
  boot1_.Query(&boot1_info, &boot1_op_size);
  boot2_.Query(&boot2_info, &boot2_op_size);

  EXPECT_EQ(boot1_info.block_count, (0x10 * 128 * 1024) / FakeSdmmcDevice::kBlockSize);
  EXPECT_EQ(boot2_info.block_count, (0x10 * 128 * 1024) / FakeSdmmcDevice::kBlockSize);

  EXPECT_EQ(boot1_info.block_size, FakeSdmmcDevice::kBlockSize);
  EXPECT_EQ(boot2_info.block_size, FakeSdmmcDevice::kBlockSize);

  EXPECT_EQ(boot1_op_size, kBlockOpSize);
  EXPECT_EQ(boot2_op_size, kBlockOpSize);
}

TEST_F(SdmmcBlockDeviceTest, AccessBootPartitions) {
  AddDevice();

  ASSERT_TRUE(boot1_.is_valid());
  ASSERT_TRUE(boot2_.is_valid());

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, 0, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 5, 10, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 10, 500, &op3));

  FillVmo(op1->private_storage()->mapper, 1);
  FillSdmmc(5, 10);
  FillVmo(op3->private_storage()->mapper, 10);

  CallbackContext ctx(1);

  sdmmc_.set_command_callback(MMC_SWITCH, [](sdmmc_req_t* req) {
    const uint32_t index = (req->arg >> 16) & 0xff;
    const uint32_t value = (req->arg >> 8) & 0xff;
    EXPECT_EQ(index, MMC_EXT_CSD_PARTITION_CONFIG);
    EXPECT_EQ(value, 0xa8 | BOOT_PARTITION_1);
  });

  boot1_.Queue(op1->operation(), OperationCallback, &ctx);
  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  ctx.expected_operations = 1;
  sync_completion_reset(&ctx.completion);

  sdmmc_.set_command_callback(MMC_SWITCH, [](sdmmc_req_t* req) {
    const uint32_t index = (req->arg >> 16) & 0xff;
    const uint32_t value = (req->arg >> 8) & 0xff;
    EXPECT_EQ(index, MMC_EXT_CSD_PARTITION_CONFIG);
    EXPECT_EQ(value, 0xa8 | BOOT_PARTITION_2);
  });

  boot2_.Queue(op2->operation(), OperationCallback, &ctx);
  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  ctx.expected_operations = 1;
  sync_completion_reset(&ctx.completion);

  sdmmc_.set_command_callback(MMC_SWITCH, [](sdmmc_req_t* req) {
    const uint32_t index = (req->arg >> 16) & 0xff;
    const uint32_t value = (req->arg >> 8) & 0xff;
    EXPECT_EQ(index, MMC_EXT_CSD_PARTITION_CONFIG);
    EXPECT_EQ(value, 0xa8 | USER_DATA_PARTITION);
  });

  user_.Queue(op3->operation(), OperationCallback, &ctx);
  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  EXPECT_TRUE(op1->private_storage()->completed);
  EXPECT_TRUE(op2->private_storage()->completed);
  EXPECT_TRUE(op3->private_storage()->completed);

  EXPECT_OK(op1->private_storage()->status);
  EXPECT_OK(op2->private_storage()->status);
  EXPECT_OK(op3->private_storage()->status);

  ASSERT_NO_FATAL_FAILURES(CheckSdmmc(1, 0));
  ASSERT_NO_FATAL_FAILURES(CheckVmo(op2->private_storage()->mapper, 5));
  ASSERT_NO_FATAL_FAILURES(CheckSdmmc(10, 500));
}

TEST_F(SdmmcBlockDeviceTest, BootPartitionRepeatedAccess) {
  AddDevice();

  ASSERT_TRUE(boot2_.is_valid());

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 1, 0, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 5, 10, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 2, 5, &op3));

  FillSdmmc(1, 0);
  FillVmo(op2->private_storage()->mapper, 5);
  FillVmo(op3->private_storage()->mapper, 2);

  CallbackContext ctx(1);

  sdmmc_.set_command_callback(MMC_SWITCH, [](sdmmc_req_t* req) {
    const uint32_t index = (req->arg >> 16) & 0xff;
    const uint32_t value = (req->arg >> 8) & 0xff;
    EXPECT_EQ(index, MMC_EXT_CSD_PARTITION_CONFIG);
    EXPECT_EQ(value, 0xa8 | BOOT_PARTITION_2);
  });

  boot2_.Queue(op1->operation(), OperationCallback, &ctx);
  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  ctx.expected_operations = 2;
  sync_completion_reset(&ctx.completion);

  // Repeated accesses to one partition should not generate more than one MMC_SWITCH command.
  sdmmc_.set_command_callback(MMC_SWITCH, [](sdmmc_req_t* req) { FAIL(); });

  boot2_.Queue(op2->operation(), OperationCallback, &ctx);
  boot2_.Queue(op3->operation(), OperationCallback, &ctx);

  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  EXPECT_TRUE(op1->private_storage()->completed);
  EXPECT_TRUE(op2->private_storage()->completed);
  EXPECT_TRUE(op3->private_storage()->completed);

  EXPECT_OK(op1->private_storage()->status);
  EXPECT_OK(op2->private_storage()->status);
  EXPECT_OK(op3->private_storage()->status);

  ASSERT_NO_FATAL_FAILURES(CheckVmo(op1->private_storage()->mapper, 1));
  ASSERT_NO_FATAL_FAILURES(CheckSdmmc(5, 10));
  ASSERT_NO_FATAL_FAILURES(CheckSdmmc(2, 5));
}

TEST_F(SdmmcBlockDeviceTest, AccessBootPartitionOutOfRange) {
  AddDevice();

  ASSERT_TRUE(boot1_.is_valid());

  std::optional<block::Operation<OperationContext>> op1;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, 4096, &op1));

  std::optional<block::Operation<OperationContext>> op2;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 8, 4088, &op2));

  std::optional<block::Operation<OperationContext>> op3;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 9, 4088, &op3));

  std::optional<block::Operation<OperationContext>> op4;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 16, 4088, &op4));

  std::optional<block::Operation<OperationContext>> op5;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_READ, 0, 2048, &op5));

  std::optional<block::Operation<OperationContext>> op6;
  ASSERT_NO_FATAL_FAILURES(MakeBlockOp(BLOCK_OP_WRITE, 1, 4095, &op6));

  CallbackContext ctx(6);

  boot1_.Queue(op1->operation(), OperationCallback, &ctx);
  boot1_.Queue(op2->operation(), OperationCallback, &ctx);
  boot1_.Queue(op3->operation(), OperationCallback, &ctx);
  boot1_.Queue(op4->operation(), OperationCallback, &ctx);
  boot1_.Queue(op5->operation(), OperationCallback, &ctx);
  boot1_.Queue(op6->operation(), OperationCallback, &ctx);

  EXPECT_OK(sync_completion_wait(&ctx.completion, zx::duration::infinite().get()));

  EXPECT_TRUE(op1->private_storage()->completed);
  EXPECT_TRUE(op2->private_storage()->completed);
  EXPECT_TRUE(op3->private_storage()->completed);
  EXPECT_TRUE(op4->private_storage()->completed);
  EXPECT_TRUE(op5->private_storage()->completed);
  EXPECT_TRUE(op6->private_storage()->completed);

  EXPECT_NOT_OK(op1->private_storage()->status);
  EXPECT_OK(op2->private_storage()->status);
  EXPECT_NOT_OK(op3->private_storage()->status);
  EXPECT_NOT_OK(op4->private_storage()->status);
  EXPECT_OK(op5->private_storage()->status);
  EXPECT_OK(op6->private_storage()->status);
}

TEST_F(SdmmcBlockDeviceTest, ProbeUsesPrefsHs) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    uint8_t* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    ext_csd[MMC_EXT_CSD_DEVICE_TYPE] = 0b0101'0110;  // Card supports HS200/400, HS/DDR.
    ext_csd[MMC_EXT_CSD_PARTITION_SWITCH_TIME] = 0;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 0;
  });

  sdmmc_.set_host_info({
      .prefs = SDMMC_HOST_PREFS_DISABLE_HS200 | SDMMC_HOST_PREFS_DISABLE_HS400 |
               SDMMC_HOST_PREFS_DISABLE_HSDDR,
  });

  SdmmcBlockDevice dut(nullptr, SdmmcDevice(sdmmc_.GetClient()));
  EXPECT_OK(dut.Init());
  EXPECT_OK(dut.ProbeMmc());

  EXPECT_EQ(sdmmc_.timing(), SDMMC_TIMING_HS);
}

TEST_F(SdmmcBlockDeviceTest, ProbeUsesPrefsHsDdr) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    uint8_t* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    ext_csd[MMC_EXT_CSD_DEVICE_TYPE] = 0b0101'0110;  // Card supports HS200/400, HS/DDR.
    ext_csd[MMC_EXT_CSD_PARTITION_SWITCH_TIME] = 0;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 0;
  });

  sdmmc_.set_host_info({
      .prefs = SDMMC_HOST_PREFS_DISABLE_HS200 | SDMMC_HOST_PREFS_DISABLE_HS400,
  });

  SdmmcBlockDevice dut(nullptr, SdmmcDevice(sdmmc_.GetClient()));
  EXPECT_OK(dut.Init());
  EXPECT_OK(dut.ProbeMmc());

  EXPECT_EQ(sdmmc_.timing(), SDMMC_TIMING_HSDDR);
}

TEST_F(SdmmcBlockDeviceTest, ProbeSd) {
  sdmmc_.set_command_callback(SD_SEND_IF_COND,
                              [](sdmmc_req_t* req) { req->response[0] = req->arg & 0xfff; });

  sdmmc_.set_command_callback(SD_APP_SEND_OP_COND, [](sdmmc_req_t* req) {
    req->response[0] = 0xc000'0000;  // Set busy and CCS bits.
  });

  sdmmc_.set_command_callback(SD_SEND_RELATIVE_ADDR, [](sdmmc_req_t* req) {
    req->response[0] = 0x100;  // Set READY_FOR_DATA bit in SD status.
  });

  sdmmc_.set_command_callback(SDMMC_SEND_CSD, [](sdmmc_req_t* req) {
    req->response[1] = 0x1234'0000;
    req->response[2] = 0x0000'5678;
    req->response[3] = 0x4000'0000;  // Set CSD_STRUCTURE to indicate SDHC/SDXC.
  });

  EXPECT_OK(dut_.ProbeSd());

  EXPECT_OK(dut_.AddDevice());

  ddk::BlockImplProtocolClient user = GetBlockClient(USER_DATA_PARTITION);
  ASSERT_TRUE(user.is_valid());

  size_t block_op_size;
  block_info_t info;
  user.Query(&info, &block_op_size);

  EXPECT_EQ(info.block_size, 512);
  EXPECT_EQ(info.block_count, 0x38'1235 * 1024ul);
}

TEST_F(SdmmcBlockDeviceTest, RpmbPartition) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    uint8_t* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    ext_csd[MMC_EXT_CSD_RPMB_SIZE_MULT] = 0x74;
    ext_csd[MMC_EXT_CSD_PARTITION_CONFIG] = 0xa8;
    ext_csd[MMC_EXT_CSD_REL_WR_SEC_C] = 1;
    ext_csd[MMC_EXT_CSD_BOOT_SIZE_MULT] = 0x10;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 0;
  });

  AddDevice();

  fake_ddk::FidlMessenger messenger;
  const auto message_op = [](void* ctx, fidl_incoming_msg_t* msg, fidl_txn_t* txn) -> zx_status_t {
    return static_cast<decltype(ddk_)*>(ctx)->MessageChild(1, msg, txn);
  };
  ASSERT_OK(messenger.SetMessageOp(&ddk_, message_op));

  sync_completion_t completion;
  rpmb_fidl_->GetDeviceInfo(
      [&](fidl::WireResponse<fuchsia_hardware_rpmb::Rpmb::GetDeviceInfo>* response) {
        EXPECT_TRUE(response->info.is_emmc_info());
        EXPECT_EQ(response->info.emmc_info().rpmb_size, 0x74);
        EXPECT_EQ(response->info.emmc_info().reliable_write_sector_count, 1);
        sync_completion_signal(&completion);
      });

  sync_completion_wait(&completion, zx::duration::infinite().get());
  sync_completion_reset(&completion);

  fzl::VmoMapper tx_frames_mapper;
  fzl::VmoMapper rx_frames_mapper;

  zx::vmo tx_frames;
  zx::vmo rx_frames;

  ASSERT_OK(tx_frames_mapper.CreateAndMap(512 * 4, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, nullptr,
                                          &tx_frames));
  ASSERT_OK(rx_frames_mapper.CreateAndMap(512 * 4, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, nullptr,
                                          &rx_frames));

  fuchsia_hardware_rpmb::wire::Request write_read_request = {};
  ASSERT_OK(tx_frames.duplicate(ZX_RIGHT_READ | ZX_RIGHT_TRANSFER | ZX_RIGHT_MAP,
                                &write_read_request.tx_frames.vmo));

  write_read_request.tx_frames.offset = 1024;
  write_read_request.tx_frames.size = 1024;
  FillVmo(tx_frames_mapper, 2, 2);

  fuchsia_mem::wire::Range rx_frames_range = {};
  ASSERT_OK(rx_frames.duplicate(ZX_RIGHT_SAME_RIGHTS, &rx_frames_range.vmo));
  rx_frames_range.offset = 512;
  rx_frames_range.size = 1536;
  write_read_request.rx_frames =
      fidl::ObjectView<fuchsia_mem::wire::Range>::FromExternal(&rx_frames_range);

  sdmmc_.set_command_callback(MMC_SWITCH, [](sdmmc_req_t* req) {
    const uint32_t index = (req->arg >> 16) & 0xff;
    const uint32_t value = (req->arg >> 8) & 0xff;
    EXPECT_EQ(index, MMC_EXT_CSD_PARTITION_CONFIG);
    EXPECT_EQ(value, 0xa8 | RPMB_PARTITION);
  });

  rpmb_fidl_->Request(std::move(write_read_request),
                      [&](fidl::WireResponse<fuchsia_hardware_rpmb::Rpmb::Request>* response) {
                        EXPECT_FALSE(response->result.is_err());
                        sync_completion_signal(&completion);
                      });

  sync_completion_wait(&completion, zx::duration::infinite().get());
  sync_completion_reset(&completion);

  ASSERT_NO_FATAL_FAILURES(CheckSdmmc(2, 0));
  // The first two blocks were written by the RPMB write request, and read back by the read request.
  ASSERT_NO_FATAL_FAILURES(CheckVmo(rx_frames_mapper, 2, 1));

  fuchsia_hardware_rpmb::wire::Request write_request = {};
  ASSERT_OK(tx_frames.duplicate(ZX_RIGHT_READ | ZX_RIGHT_TRANSFER | ZX_RIGHT_MAP,
                                &write_request.tx_frames.vmo));

  write_request.tx_frames.offset = 0;
  write_request.tx_frames.size = 2048;
  FillVmo(tx_frames_mapper, 4, 0);

  // Repeated accesses to one partition should not generate more than one MMC_SWITCH command.
  sdmmc_.set_command_callback(MMC_SWITCH, [](__UNUSED sdmmc_req_t* req) { FAIL(); });

  sdmmc_.set_command_callback(SDMMC_SET_BLOCK_COUNT, [](sdmmc_req_t* req) {
    EXPECT_TRUE(req->arg & MMC_SET_BLOCK_COUNT_RELIABLE_WRITE);
  });

  rpmb_fidl_->Request(std::move(write_request),
                      [&](fidl::WireResponse<fuchsia_hardware_rpmb::Rpmb::Request>* response) {
                        EXPECT_FALSE(response->result.is_err());
                        sync_completion_signal(&completion);
                      });

  sync_completion_wait(&completion, zx::duration::infinite().get());
  sync_completion_reset(&completion);

  ASSERT_NO_FATAL_FAILURES(CheckSdmmc(4, 0));
}

TEST_F(SdmmcBlockDeviceTest, RpmbRequestLimit) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    uint8_t* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    ext_csd[MMC_EXT_CSD_RPMB_SIZE_MULT] = 0x74;
    ext_csd[MMC_EXT_CSD_PARTITION_CONFIG] = 0xa8;
    ext_csd[MMC_EXT_CSD_REL_WR_SEC_C] = 1;
    ext_csd[MMC_EXT_CSD_BOOT_SIZE_MULT] = 0x10;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 0;
  });

  AddDevice();
  dut_.StopWorkerThread();

  zx::vmo tx_frames;
  ASSERT_OK(zx::vmo::create(512, 0, &tx_frames));

  for (int i = 0; i < 16; i++) {
    fuchsia_hardware_rpmb::wire::Request request = {};
    ASSERT_OK(tx_frames.duplicate(ZX_RIGHT_SAME_RIGHTS, &request.tx_frames.vmo));
    request.tx_frames.offset = 0;
    request.tx_frames.size = 512;
    rpmb_fidl_->Request(
        std::move(request),
        [&](__UNUSED fidl::WireResponse<fuchsia_hardware_rpmb::Rpmb::Request>* response) {});
  }

  fuchsia_hardware_rpmb::wire::Request error_request = {};
  ASSERT_OK(tx_frames.duplicate(ZX_RIGHT_SAME_RIGHTS, &error_request.tx_frames.vmo));
  error_request.tx_frames.offset = 0;
  error_request.tx_frames.size = 512;

  sync_completion_t error_completion;
  rpmb_fidl_->Request(std::move(error_request),
                      [&](fidl::WireResponse<fuchsia_hardware_rpmb::Rpmb::Request>* response) {
                        EXPECT_TRUE(response->result.is_err());
                        sync_completion_signal(&error_completion);
                      });

  sync_completion_wait(&error_completion, zx::duration::infinite().get());
}

void SdmmcBlockDeviceTest::QueueBlockOps() {
  struct BlockContext {
    sync_completion_t completion = {};
    block::OperationPool<> free_ops;
    block::OperationQueue<> outstanding_ops;
    std::atomic<uint32_t> outstanding_op_count = 0;
  } context;

  const auto op_callback = [](void* ctx, zx_status_t status, block_op_t* bop) {
    EXPECT_OK(status);

    auto& op_ctx = *reinterpret_cast<BlockContext*>(ctx);
    block::Operation<> op(bop, kBlockOpSize);
    EXPECT_TRUE(op_ctx.outstanding_ops.erase(&op));
    op_ctx.free_ops.push(std::move(op));

    // Wake up the block op thread when half the outstanding operations have been completed.
    if (op_ctx.outstanding_op_count.fetch_sub(1) == kMaxOutstandingOps / 2) {
      sync_completion_signal(&op_ctx.completion);
    }
  };

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(1024, 0, &vmo));

  // Populate the free op list.
  for (uint32_t i = 0; i < kMaxOutstandingOps; i++) {
    std::optional<block::Operation<>> op = block::Operation<>::Alloc(kBlockOpSize);
    ASSERT_TRUE(op);
    context.free_ops.push(*std::move(op));
  }

  while (run_threads_.load()) {
    for (uint32_t i = context.outstanding_op_count.load(); i < kMaxOutstandingOps;
         i = context.outstanding_op_count.fetch_add(1) + 1) {
      // Move an op from the free list to the outstanding list. The callback will erase the op from
      // the outstanding list and move it back to the free list.
      std::optional<block::Operation<>> op = context.free_ops.pop();
      ASSERT_TRUE(op);

      op->operation()->rw = {.command = BLOCK_OP_READ, .vmo = vmo.get(), .length = 1};

      block_op_t* const bop = op->operation();
      context.outstanding_ops.push(*std::move(op));
      user_.Queue(bop, op_callback, &context);
    }

    sync_completion_wait(&context.completion, zx::duration::infinite().get());
    sync_completion_reset(&context.completion);
  }

  while (context.outstanding_op_count.load() > 0) {
  }
}

void SdmmcBlockDeviceTest::QueueRpmbRequests() {
  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(512, 0, &vmo));

  std::atomic<uint32_t> outstanding_op_count = 0;
  sync_completion_t completion;

  while (run_threads_.load()) {
    for (uint32_t i = outstanding_op_count.load(); i < kMaxOutstandingOps;
         i = outstanding_op_count.fetch_add(1) + 1) {
      fuchsia_hardware_rpmb::wire::Request request = {};
      EXPECT_OK(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &request.tx_frames.vmo));
      request.tx_frames.offset = 0;
      request.tx_frames.size = 512;

      rpmb_fidl_->Request(std::move(request),
                          [&](fidl::WireResponse<fuchsia_hardware_rpmb::Rpmb::Request>* response) {
                            EXPECT_FALSE(response->result.is_err());
                            if (outstanding_op_count.fetch_sub(1) == kMaxOutstandingOps / 2) {
                              sync_completion_signal(&completion);
                            }
                          });
    }

    sync_completion_wait(&completion, zx::duration::infinite().get());
    sync_completion_reset(&completion);
  }

  while (outstanding_op_count.load() > 0) {
  }
}

TEST_F(SdmmcBlockDeviceTest, RpmbRequestsGetToRun) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    auto* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    *reinterpret_cast<uint32_t*>(&ext_csd[212]) = htole32(kBlockCount);

    ext_csd[MMC_EXT_CSD_RPMB_SIZE_MULT] = 0x10;
    ext_csd[MMC_EXT_CSD_PARTITION_CONFIG] = 0xa8;
    ext_csd[MMC_EXT_CSD_PARTITION_SWITCH_TIME] = 0;
    ext_csd[MMC_EXT_CSD_BOOT_SIZE_MULT] = 0x10;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 0;
  });

  AddDevice();

  ASSERT_TRUE(boot1_.is_valid());
  ASSERT_TRUE(boot2_.is_valid());

  thrd_t block_thread;
  EXPECT_EQ(thrd_create_with_name(
                &block_thread,
                [](void* ctx) -> int {
                  reinterpret_cast<SdmmcBlockDeviceTest*>(ctx)->QueueBlockOps();
                  return thrd_success;
                },
                this, "block-queue-thread"),
            thrd_success);

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(512, 0, &vmo));

  std::atomic<uint32_t> ops_completed = 0;
  sync_completion_t completion;

  for (uint32_t i = 0; i < kMaxOutstandingOps; i++) {
    fuchsia_hardware_rpmb::wire::Request request = {};
    EXPECT_OK(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &request.tx_frames.vmo));
    request.tx_frames.offset = 0;
    request.tx_frames.size = 512;

    rpmb_fidl_->Request(std::move(request),
                        [&](fidl::WireResponse<fuchsia_hardware_rpmb::Rpmb::Request>* response) {
                          EXPECT_FALSE(response->result.is_err());
                          if ((ops_completed.fetch_add(1) + 1) == kMaxOutstandingOps) {
                            sync_completion_signal(&completion);
                          }
                        });
  }

  sync_completion_wait(&completion, zx::duration::infinite().get());

  run_threads_.store(false);
  EXPECT_EQ(thrd_join(block_thread, nullptr), thrd_success);
}

TEST_F(SdmmcBlockDeviceTest, BlockOpsGetToRun) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    auto* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    *reinterpret_cast<uint32_t*>(&ext_csd[212]) = htole32(kBlockCount);

    ext_csd[MMC_EXT_CSD_RPMB_SIZE_MULT] = 0x10;
    ext_csd[MMC_EXT_CSD_PARTITION_CONFIG] = 0xa8;
    ext_csd[MMC_EXT_CSD_PARTITION_SWITCH_TIME] = 0;
    ext_csd[MMC_EXT_CSD_BOOT_SIZE_MULT] = 0x10;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 0;
  });

  AddDevice();

  ASSERT_TRUE(boot1_.is_valid());
  ASSERT_TRUE(boot2_.is_valid());

  thrd_t rpmb_thread;
  EXPECT_EQ(thrd_create_with_name(
                &rpmb_thread,
                [](void* ctx) -> int {
                  reinterpret_cast<SdmmcBlockDeviceTest*>(ctx)->QueueRpmbRequests();
                  return thrd_success;
                },
                this, "rpmb-queue-thread"),
            thrd_success);

  block::OperationPool<> outstanding_ops;

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(1024, 0, &vmo));

  struct BlockContext {
    std::atomic<uint32_t> ops_completed = 0;
    sync_completion_t completion = {};
  } context;

  const auto op_callback = [](void* ctx, zx_status_t status, __UNUSED block_op_t* bop) {
    EXPECT_OK(status);

    auto& op_ctx = *reinterpret_cast<BlockContext*>(ctx);
    if ((op_ctx.ops_completed.fetch_add(1) + 1) == kMaxOutstandingOps) {
      sync_completion_signal(&op_ctx.completion);
    }
  };

  for (uint32_t i = 0; i < kMaxOutstandingOps; i++) {
    std::optional<block::Operation<>> op = block::Operation<>::Alloc(kBlockOpSize);
    ASSERT_TRUE(op);

    op->operation()->rw = {.command = BLOCK_OP_READ, .vmo = vmo.get(), .length = 1};

    block_op_t* const bop = op->operation();
    outstanding_ops.push(*std::move(op));
    user_.Queue(bop, op_callback, &context);
  }

  sync_completion_wait(&context.completion, zx::duration::infinite().get());

  run_threads_.store(false);
  EXPECT_EQ(thrd_join(rpmb_thread, nullptr), thrd_success);
}

TEST_F(SdmmcBlockDeviceTest, GetRpmbClient) {
  sdmmc_.set_command_callback(MMC_SEND_EXT_CSD, [](sdmmc_req_t* req) {
    auto* const ext_csd = reinterpret_cast<uint8_t*>(req->virt_buffer);
    *reinterpret_cast<uint32_t*>(&ext_csd[212]) = htole32(kBlockCount);

    ext_csd[MMC_EXT_CSD_RPMB_SIZE_MULT] = 0x74;
    ext_csd[MMC_EXT_CSD_PARTITION_CONFIG] = 0xa8;
    ext_csd[MMC_EXT_CSD_REL_WR_SEC_C] = 1;
    ext_csd[MMC_EXT_CSD_PARTITION_SWITCH_TIME] = 0;
    ext_csd[MMC_EXT_CSD_BOOT_SIZE_MULT] = 0x10;
    ext_csd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 0;
  });

  AddDevice();

  ASSERT_TRUE(rpmb_.is_valid());

  zx::status rpmb_ends = fidl::CreateEndpoints<fuchsia_hardware_rpmb::Rpmb>();
  ASSERT_OK(rpmb_ends.status_value());
  rpmb_.ConnectServer(rpmb_ends->server.TakeChannel());

  fidl::WireSharedClient rpmb_client(std::move(rpmb_ends->client), loop_.dispatcher());

  sync_completion_t completion;
  rpmb_client->GetDeviceInfo(
      [&](fidl::WireResponse<fuchsia_hardware_rpmb::Rpmb::GetDeviceInfo>* response) {
        EXPECT_TRUE(response->info.is_emmc_info());
        EXPECT_EQ(response->info.emmc_info().rpmb_size, 0x74);
        EXPECT_EQ(response->info.emmc_info().reliable_write_sector_count, 1);
        sync_completion_signal(&completion);
      });

  sync_completion_wait(&completion, zx::duration::infinite().get());
  sync_completion_reset(&completion);
}

}  // namespace sdmmc

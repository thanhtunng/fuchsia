// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/device/llcpp/fidl.h>
#include <fuchsia/hardware/audio/llcpp/fidl.h>
#include <lib/fake_ddk/fake_ddk.h>
#include <lib/simple-audio-stream/simple-audio-stream.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/clock.h>
#include <threads.h>

#include <set>

#include <audio-proto-utils/format-utils.h>
#include <sdk/lib/inspect/testing/cpp/zxtest/inspect.h>
#include <zxtest/zxtest.h>

namespace audio {

namespace audio_fidl = fuchsia_hardware_audio;

audio_fidl::wire::PcmFormat GetDefaultPcmFormat() {
  audio_fidl::wire::PcmFormat format;
  format.number_of_channels = 2;
  format.channels_to_use_bitmask = 0x03;
  format.sample_format = audio_fidl::wire::SampleFormat::kPcmSigned;
  format.frame_rate = 48000;
  format.bytes_per_sample = 2;
  format.valid_bits_per_sample = 16;
  return format;
}

class SimpleAudioTest : public inspect::InspectTestHelper, public zxtest::Test {
 public:
  void SetUp() override {}

  void TearDown() override {}

  template <typename T>
  static void CheckPropertyNotEqual(const inspect::NodeValue& node, std::string property,
                                    T not_expected_value) {
    const T* actual_value = node.get_property<T>(property);
    EXPECT_TRUE(actual_value);
    if (!actual_value) {
      return;
    }
    EXPECT_NE(not_expected_value.value(), actual_value->value());
  }

 protected:
  fake_ddk::Bind ddk_;
};

class MockSimpleAudio : public SimpleAudioStream {
 public:
  static constexpr uint32_t kTestFrameRate = 48000;
  static constexpr uint8_t kTestNumberOfChannels = 2;
  static constexpr uint32_t kTestFifoDepth = 16;
  static constexpr uint32_t kTestClockDomain = audio_fidl::wire::kClockDomainExternal;
  static constexpr uint32_t kTestPositionNotify = 4;
  static constexpr float kTestGain = 1.2345f;

  MockSimpleAudio(zx_device_t* parent) : SimpleAudioStream(parent, false /* is input */) {}

  void PostSetPlugState(bool plugged, zx::duration delay) {
    async::PostDelayedTask(
        dispatcher(),
        [this, plugged]() {
          ScopedToken t(domain_token());
          SetPlugState(plugged);
        },
        delay);
  }
  inspect::Inspector& inspect() { return SimpleAudioStream::inspect(); }

 protected:
  zx_status_t Init() __TA_REQUIRES(domain_token()) override {
    fbl::AllocChecker ac;
    supported_formats_.reserve(1, &ac);
    if (!ac.check()) {
      return ZX_ERR_NO_MEMORY;
    }
    SimpleAudioStream::SupportedFormat format = {};
    format.range.min_channels = kTestNumberOfChannels;
    format.range.max_channels = kTestNumberOfChannels;
    format.range.sample_formats = AUDIO_SAMPLE_FORMAT_16BIT;
    format.range.min_frames_per_second = kTestFrameRate;
    format.range.max_frames_per_second = kTestFrameRate;
    format.range.flags = ASF_RANGE_FLAG_FPS_48000_FAMILY;

    format.frequency_ranges.push_back({
        .min_frequency = 40,
        .max_frequency = 3'000,
    });
    format.frequency_ranges.push_back({
        .min_frequency = 3'000,
        .max_frequency = 25'000,
    });

    supported_formats_.push_back(std::move(format));

    fifo_depth_ = kTestFifoDepth;
    clock_domain_ = kTestClockDomain;

    // Set our gain capabilities.
    cur_gain_state_.cur_gain = 0;
    cur_gain_state_.cur_mute = false;
    cur_gain_state_.cur_agc = false;
    cur_gain_state_.min_gain = 0;
    cur_gain_state_.max_gain = 100;
    cur_gain_state_.gain_step = 0;
    cur_gain_state_.can_mute = true;
    cur_gain_state_.can_agc = true;

    SetInitialPlugState(AUDIO_PDNF_CAN_NOTIFY);

    snprintf(device_name_, sizeof(device_name_), "test-audio-in");
    snprintf(mfr_name_, sizeof(mfr_name_), "Bike Sheds, Inc.");
    snprintf(prod_name_, sizeof(prod_name_), "testy_mctestface");

    unique_id_ = AUDIO_STREAM_UNIQUE_ID_BUILTIN_MICROPHONE;

    return ZX_OK;
  }

  zx_status_t SetGain(const audio_proto::SetGainReq& req) __TA_REQUIRES(domain_token()) override {
    if (req.flags & AUDIO_SGF_GAIN_VALID) {
      cur_gain_state_.cur_gain = req.gain;
    }
    if (req.flags & AUDIO_SGF_AGC_VALID) {
      cur_gain_state_.cur_agc = req.flags & AUDIO_SGF_AGC;
    }
    if (req.flags & AUDIO_SGF_MUTE_VALID) {
      cur_gain_state_.cur_mute = req.flags & AUDIO_SGF_MUTE;
    }
    return ZX_OK;
  }

  zx_status_t ChangeFormat(const audio_proto::StreamSetFmtReq& req)
      __TA_REQUIRES(domain_token()) override {
    return ZX_OK;
  }

  zx_status_t GetBuffer(const audio_proto::RingBufGetBufferReq& req, uint32_t* out_num_rb_frames,
                        zx::vmo* out_buffer) __TA_REQUIRES(domain_token()) override {
    zx::vmo rb;
    *out_num_rb_frames = req.min_ring_buffer_frames;
    zx::vmo::create(*out_num_rb_frames * 2 * 2, 0, &rb);
    us_per_notification_ = 1'000 * MockSimpleAudio::kTestFrameRate / *out_num_rb_frames * 1'000 /
                           req.notifications_per_ring;
    constexpr uint32_t rights = ZX_RIGHT_READ | ZX_RIGHT_WRITE | ZX_RIGHT_MAP | ZX_RIGHT_TRANSFER;
    return rb.duplicate(rights, out_buffer);
  }

  zx_status_t Start(uint64_t* out_start_time) __TA_REQUIRES(domain_token()) override {
    *out_start_time = zx::clock::get_monotonic().get();
    notify_timer_.PostDelayed(dispatcher(), zx::usec(us_per_notification_));
    return ZX_OK;
  }

  zx_status_t Stop() __TA_REQUIRES(domain_token()) override {
    notify_timer_.Cancel();
    return ZX_OK;
  }
  zx_status_t ChangeActiveChannels(uint64_t mask) __TA_REQUIRES(domain_token()) override {
    return ZX_ERR_NOT_SUPPORTED;
  }

  void ProcessRingNotification() {
    ScopedToken t(domain_token());
    audio_proto::RingBufPositionNotify resp = {};
    resp.hdr.cmd = AUDIO_RB_POSITION_NOTIFY;
    resp.monotonic_time = zx::clock::get_monotonic().get();
    resp.ring_buffer_pos = kTestPositionNotify;
    NotifyPosition(resp);
    notify_timer_.PostDelayed(dispatcher(), zx::usec(us_per_notification_));
  }

  void ShutdownHook() __TA_REQUIRES(domain_token()) override { Stop(); }

 private:
  async::TaskClosureMethod<MockSimpleAudio, &MockSimpleAudio::ProcessRingNotification> notify_timer_
      TA_GUARDED(domain_token()){this};

  uint32_t us_per_notification_ = 0;
};

TEST_F(SimpleAudioTest, DdkLifeCycleTest) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);
  ddk::SuspendTxn txn(server->zxdev(), 0, false, DEVICE_SUSPEND_REASON_SELECTIVE_SUSPEND);
  server->DdkSuspend(std::move(txn));
  EXPECT_FALSE(ddk_.remove_called());
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, UnbindAndAlsoShutdown) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);
  server->DdkAsyncRemove();
  server->Shutdown();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, SetAndGetGain) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  {
    fidl::Arena allocator;
    audio_fidl::wire::GainState gain_state(allocator);
    gain_state.set_gain_db(allocator, MockSimpleAudio::kTestGain);
    auto status =
        fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).SetGain(std::move(gain_state));
    ASSERT_OK(status.status());
  }

  auto gain_state = fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).WatchGainState();
  ASSERT_OK(gain_state.status());
  ASSERT_EQ(MockSimpleAudio::kTestGain, gain_state->gain_state.gain_db());
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, WatchGainAndCloseStreamBeforeReply) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  {
    fidl::Arena allocator;
    audio_fidl::wire::GainState gain_state(allocator);
    gain_state.set_gain_db(allocator, MockSimpleAudio::kTestGain);
    auto status =
        fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).SetGain(std::move(gain_state));
    ASSERT_OK(status.status());
  }

  // One watch for initial reply.
  auto gain_state = fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).WatchGainState();
  ASSERT_OK(gain_state.status());
  ASSERT_EQ(MockSimpleAudio::kTestGain, gain_state->gain_state.gain_db());

  // A second watch with no reply since there is no change of gain.
  auto f = [](void* arg) -> int {
    auto ch = static_cast<fidl::WireResult<audio_fidl::Device::GetChannel>*>(arg);
    fidl::WireCall<audio_fidl::StreamConfig>((*ch)->channel).WatchGainState();
    return 0;
  };
  thrd_t th;
  ASSERT_OK(thrd_create_with_name(&th, f, &ch, "test-thread"));

  // We want the watch to be started before we reset the channel triggering a deactivation.
  zx::nanosleep(zx::deadline_after(zx::msec(100)));
  ch->channel.reset();

  int result = -1;
  thrd_join(th, &result);
  ASSERT_EQ(result, 0);
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, SetAndGetAgc) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  {
    fidl::Arena allocator;
    audio_fidl::wire::GainState gain_state(allocator);
    gain_state.set_agc_enabled(allocator, true);
    auto status =
        fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).SetGain(std::move(gain_state));
    ASSERT_OK(status.status());
  }

  auto gain_state1 = fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).WatchGainState();
  ASSERT_OK(gain_state1.status());
  ASSERT_TRUE(gain_state1->gain_state.agc_enabled());

  {
    fidl::Arena allocator;
    audio_fidl::wire::GainState gain_state(allocator);
    gain_state.set_agc_enabled(allocator, false);
    auto status =
        fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).SetGain(std::move(gain_state));
    ASSERT_OK(status.status());
  }

  auto gain_state2 = fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).WatchGainState();
  ASSERT_OK(gain_state2.status());
  ASSERT_FALSE(gain_state2->gain_state.agc_enabled());
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, SetAndGetMute) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  {
    fidl::Arena allocator;
    audio_fidl::wire::GainState gain_state(allocator);
    gain_state.set_muted(allocator, true);
    auto status =
        fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).SetGain(std::move(gain_state));
    ASSERT_OK(status.status());
  }

  auto gain_state1 = fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).WatchGainState();
  ASSERT_OK(gain_state1.status());
  ASSERT_TRUE(gain_state1->gain_state.muted());

  {
    fidl::Arena allocator;
    audio_fidl::wire::GainState gain_state(allocator);
    gain_state.set_muted(allocator, false);
    auto status =
        fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).SetGain(std::move(gain_state));
    ASSERT_OK(status.status());
  }

  auto gain_state2 = fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).WatchGainState();
  ASSERT_OK(gain_state2.status());
  ASSERT_FALSE(gain_state2->gain_state.muted());
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, SetMuteWhenDisabled) {
  struct MockSimpleAudioLocal : public MockSimpleAudio {
    MockSimpleAudioLocal(zx_device_t* parent) : MockSimpleAudio(parent) {}
    zx_status_t Init() __TA_REQUIRES(domain_token()) override {
      auto status = MockSimpleAudio::Init();
      cur_gain_state_.can_mute = false;
      return status;
    }
  };
  auto server = SimpleAudioStream::Create<MockSimpleAudioLocal>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  {
    fidl::Arena allocator;
    audio_fidl::wire::GainState gain_state(allocator);
    gain_state.set_muted(allocator, true);
    auto status =
        fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).SetGain(std::move(gain_state));
    ASSERT_OK(status.status());
  }

  auto gain_state1 = fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).WatchGainState();
  ASSERT_OK(gain_state1.status());
  ASSERT_FALSE(gain_state1->gain_state.has_muted());
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, Enumerate1) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client_wrap;
  *client_wrap.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client_wrap.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  fidl::WireSyncClient<audio_fidl::StreamConfig> client(std::move(ch->channel));

  auto ret = client.GetSupportedFormats();
  auto& supported_formats = ret->supported_formats;
  auto& formats = supported_formats[0].pcm_supported_formats2();
  ASSERT_EQ(1, formats.channel_sets().count());
  ASSERT_EQ(2, formats.channel_sets()[0].attributes().count());
  ASSERT_EQ(1, formats.sample_formats().count());
  ASSERT_EQ(audio_fidl::wire::SampleFormat::kPcmSigned, formats.sample_formats()[0]);
  ASSERT_EQ(1, formats.frame_rates().count());
  ASSERT_EQ(48000, formats.frame_rates()[0]);
  ASSERT_EQ(1, formats.bytes_per_sample().count());
  ASSERT_EQ(2, formats.bytes_per_sample()[0]);
  ASSERT_EQ(1, formats.valid_bits_per_sample().count());
  ASSERT_EQ(16, formats.valid_bits_per_sample()[0]);

  auto& channels_attributes = formats.channel_sets()[0].attributes();
  ASSERT_EQ(2, channels_attributes.count());
  ASSERT_EQ(40, channels_attributes[0].min_frequency());
  ASSERT_EQ(3'000, channels_attributes[0].max_frequency());
  ASSERT_EQ(3'000, channels_attributes[1].min_frequency());
  ASSERT_EQ(25'000, channels_attributes[1].max_frequency());

  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, Enumerate2) {
  struct MockSimpleAudioLocal : public MockSimpleAudio {
    MockSimpleAudioLocal(zx_device_t* parent) : MockSimpleAudio(parent) {}
    zx_status_t Init() __TA_REQUIRES(domain_token()) override {
      auto status = MockSimpleAudio::Init();

      SimpleAudioStream::SupportedFormat format1 = {};
      SimpleAudioStream::SupportedFormat format2 = {};

      format1.range.min_channels = 2;
      format1.range.max_channels = 4;
      format1.range.sample_formats = AUDIO_SAMPLE_FORMAT_24BIT_IN32;
      format1.range.min_frames_per_second = 48'000;
      format1.range.max_frames_per_second = 768'000;
      format1.range.flags = ASF_RANGE_FLAG_FPS_48000_FAMILY;

      format2.range.min_channels = 1;
      format2.range.max_channels = 1;
      format2.range.sample_formats = AUDIO_SAMPLE_FORMAT_32BIT_FLOAT;
      format2.range.min_frames_per_second = 88'200;
      format2.range.max_frames_per_second = 88'200;
      format2.range.flags =
          ASF_RANGE_FLAG_FPS_CONTINUOUS;  // Ok only because min and max fps are equal.

      supported_formats_ = fbl::Vector<SupportedFormat>{format1, format2};
      return status;
    }
  };

  auto server = SimpleAudioStream::Create<MockSimpleAudioLocal>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client_wrap;
  *client_wrap.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client_wrap.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  fidl::WireSyncClient<audio_fidl::StreamConfig> client(std::move(ch->channel));

  auto ret = client.GetSupportedFormats();
  auto& supported_formats = ret->supported_formats;
  ASSERT_EQ(2, supported_formats.count());

  auto& formats1 = supported_formats[0].pcm_supported_formats2();
  ASSERT_EQ(3, formats1.channel_sets().count());
  ASSERT_EQ(2, formats1.channel_sets()[0].attributes().count());
  ASSERT_EQ(3, formats1.channel_sets()[1].attributes().count());
  ASSERT_EQ(4, formats1.channel_sets()[2].attributes().count());
  ASSERT_EQ(1, formats1.sample_formats().count());
  ASSERT_EQ(audio_fidl::wire::SampleFormat::kPcmSigned, formats1.sample_formats()[0]);
  ASSERT_EQ(5, formats1.frame_rates().count());
  std::set<uint32_t> rates1;
  for (auto& i : formats1.frame_rates()) {
    rates1.insert(i);
  }
  ASSERT_EQ(rates1, std::set<uint32_t>({48'000, 96'000, 192'000, 384'000, 768'000}));
  ASSERT_EQ(1, formats1.bytes_per_sample().count());
  ASSERT_EQ(4, formats1.bytes_per_sample()[0]);
  ASSERT_EQ(1, formats1.valid_bits_per_sample().count());
  ASSERT_EQ(24, formats1.valid_bits_per_sample()[0]);

  auto& formats2 = supported_formats[1].pcm_supported_formats2();
  ASSERT_EQ(1, formats2.channel_sets().count());
  ASSERT_EQ(1, formats2.channel_sets()[0].attributes().count());
  ASSERT_EQ(1, formats2.sample_formats().count());
  ASSERT_EQ(audio_fidl::wire::SampleFormat::kPcmFloat, formats2.sample_formats()[0]);
  ASSERT_EQ(1, formats2.frame_rates().count());
  std::set<uint32_t> rates2;
  for (auto& i : formats2.frame_rates()) {
    rates2.insert(i);
  }
  ASSERT_EQ(rates2, std::set<uint32_t>({88'200}));
  ASSERT_EQ(1, formats2.bytes_per_sample().count());
  ASSERT_EQ(4, formats2.bytes_per_sample()[0]);
  ASSERT_EQ(1, formats2.valid_bits_per_sample().count());
  ASSERT_EQ(32, formats2.valid_bits_per_sample()[0]);
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, CreateRingBuffer1) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client_wrap;
  *client_wrap.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client_wrap.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  fidl::WireSyncClient<audio_fidl::StreamConfig> client(std::move(ch->channel));

  auto endpoints = fidl::CreateEndpoints<audio_fidl::RingBuffer>();
  ASSERT_OK(endpoints.status_value());
  auto [local, remote] = std::move(endpoints.value());

  fidl::Arena allocator;
  audio_fidl::wire::Format format(allocator);
  format.set_pcm_format(allocator, GetDefaultPcmFormat());
  client.CreateRingBuffer(std::move(format), std::move(remote));

  auto result = fidl::WireCall<audio_fidl::RingBuffer>(local).GetProperties();
  ASSERT_OK(result.status());
  ASSERT_EQ(result->properties.fifo_depth(), MockSimpleAudio::kTestFifoDepth);
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, CreateRingBuffer2) {
  struct MockSimpleAudioLocal : public MockSimpleAudio {
    MockSimpleAudioLocal(zx_device_t* parent) : MockSimpleAudio(parent) {}
    zx_status_t Init() __TA_REQUIRES(domain_token()) override {
      SimpleAudioStream::SupportedFormat format = {};
      format.range.min_channels = 1;
      format.range.max_channels = 4;
      format.range.sample_formats =
          AUDIO_SAMPLE_FORMAT_24BIT_IN32 | AUDIO_SAMPLE_FORMAT_FLAG_UNSIGNED;
      format.range.min_frames_per_second = 22050;
      format.range.max_frames_per_second = 88200;
      format.range.flags = ASF_RANGE_FLAG_FPS_44100_FAMILY;
      supported_formats_.push_back(std::move(format));
      return MockSimpleAudio::Init();
    }
  };
  auto server = SimpleAudioStream::Create<MockSimpleAudioLocal>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client_wrap;
  *client_wrap.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client_wrap.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  fidl::WireSyncClient<audio_fidl::StreamConfig> client(std::move(ch->channel));

  auto endpoints = fidl::CreateEndpoints<audio_fidl::RingBuffer>();
  ASSERT_OK(endpoints.status_value());
  auto [local, remote] = std::move(endpoints.value());

  audio_fidl::wire::PcmFormat pcm_format;
  pcm_format.number_of_channels = 4;
  pcm_format.channels_to_use_bitmask = 0x0f;
  pcm_format.sample_format = audio_fidl::wire::SampleFormat::kPcmUnsigned;
  pcm_format.frame_rate = 44100;
  pcm_format.bytes_per_sample = 4;
  pcm_format.valid_bits_per_sample = 24;

  fidl::Arena allocator;
  audio_fidl::wire::Format format(allocator);
  format.set_pcm_format(allocator, std::move(pcm_format));
  client.CreateRingBuffer(std::move(format), std::move(remote));

  auto result = fidl::WireCall<audio_fidl::RingBuffer>(local).GetProperties();
  ASSERT_OK(result.status());
  ASSERT_EQ(result->properties.fifo_depth(), MockSimpleAudio::kTestFifoDepth);
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, SetBadFormat1) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client_wrap;
  *client_wrap.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client_wrap.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  fidl::WireSyncClient<audio_fidl::StreamConfig> client(std::move(ch->channel));

  auto endpoints = fidl::CreateEndpoints<audio_fidl::RingBuffer>();
  ASSERT_OK(endpoints.status_value());
  auto [local, remote] = std::move(endpoints.value());

  // Define a pretty bad format.
  audio_fidl::wire::PcmFormat pcm_format;
  pcm_format.sample_format = audio_fidl::wire::SampleFormat::kPcmSigned;

  fidl::Arena allocator;
  audio_fidl::wire::Format format(allocator);
  format.set_pcm_format(allocator, std::move(pcm_format));

  auto result0 = client.CreateRingBuffer(std::move(format), std::move(remote));
  ASSERT_EQ(ZX_OK, result0.status());  // CreateRingBuffer is sent successfully.

  auto result1 = client.GetSupportedFormats();
  ASSERT_EQ(ZX_ERR_PEER_CLOSED, result1.status());  // With a bad format we get a channel close.

  auto result2 = fidl::WireCall<audio_fidl::RingBuffer>(local).GetProperties();
  ASSERT_EQ(ZX_ERR_PEER_CLOSED, result2.status());  // With a bad format we get a channel close.
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, SetBadFormat2) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client_wrap;
  *client_wrap.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client_wrap.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  fidl::WireSyncClient<audio_fidl::StreamConfig> client(std::move(ch->channel));

  auto endpoints = fidl::CreateEndpoints<audio_fidl::RingBuffer>();
  ASSERT_OK(endpoints.status_value());
  auto [local, remote] = std::move(endpoints.value());

  // Define an almost good format.
  audio_fidl::wire::PcmFormat pcm_format = GetDefaultPcmFormat();
  pcm_format.frame_rate = 48001;  // Bad rate.

  fidl::Arena allocator;
  audio_fidl::wire::Format format(allocator);
  format.set_pcm_format(allocator, std::move(pcm_format));

  auto result0 = client.CreateRingBuffer(std::move(format), std::move(remote));
  ASSERT_EQ(ZX_OK, result0.status());  // CreateRingBuffer is sent successfully.

  auto result1 = client.GetSupportedFormats();
  ASSERT_EQ(ZX_ERR_PEER_CLOSED, result1.status());  // With a bad format we get a channel close.

  auto result2 = fidl::WireCall<audio_fidl::RingBuffer>(local).GetProperties();
  ASSERT_EQ(ZX_ERR_PEER_CLOSED, result2.status());  // With a bad format we get a channel close.
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, GetIds) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  auto result = fidl::WireCall<audio_fidl::StreamConfig>(ch->channel).GetProperties();
  ASSERT_OK(result.status());

  audio_stream_unique_id_t mic = AUDIO_STREAM_UNIQUE_ID_BUILTIN_MICROPHONE;
  ASSERT_BYTES_EQ(result->properties.unique_id().data(), mic.data,
                  strlen(reinterpret_cast<char*>(mic.data)));
  ASSERT_BYTES_EQ(result->properties.manufacturer().data(), "Bike Sheds, Inc.",
                  strlen("Bike Sheds, Inc."));
  ASSERT_EQ(result->properties.clock_domain(), MockSimpleAudio::kTestClockDomain);
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, MultipleChannelsPlugDetectState) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  // We get 2 channels from the one FIDL channel acquired via FidlClient() using GetChannel.

  fidl::WireResult<audio_fidl::Device::GetChannel> ch1 = client.GetChannel();
  fidl::WireResult<audio_fidl::Device::GetChannel> ch2 = client.GetChannel();
  ASSERT_EQ(ch1.status(), ZX_OK);
  ASSERT_EQ(ch2.status(), ZX_OK);

  auto prop1 = fidl::WireCall<audio_fidl::StreamConfig>(ch1->channel).GetProperties();
  auto prop2 = fidl::WireCall<audio_fidl::StreamConfig>(ch2->channel).GetProperties();
  ASSERT_OK(prop1.status());
  ASSERT_OK(prop2.status());

  ASSERT_EQ(prop1->properties.plug_detect_capabilities(),
            audio_fidl::wire::PlugDetectCapabilities::kCanAsyncNotify);
  ASSERT_EQ(prop2->properties.plug_detect_capabilities(),
            audio_fidl::wire::PlugDetectCapabilities::kCanAsyncNotify);

  auto state1 = fidl::WireCall<audio_fidl::StreamConfig>(ch1->channel).WatchPlugState();
  auto state2 = fidl::WireCall<audio_fidl::StreamConfig>(ch2->channel).WatchPlugState();
  ASSERT_OK(state1.status());
  ASSERT_OK(state2.status());
  ASSERT_FALSE(state1->plug_state.plugged());
  ASSERT_FALSE(state2->plug_state.plugged());
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, WatchPlugDetectAndCloseStreamBeforeReply) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  // We get 2 channels from the one FIDL channel acquired via FidlClient() using GetChannel.
  fidl::WireResult<audio_fidl::Device::GetChannel> ch1 = client.GetChannel();
  fidl::WireResult<audio_fidl::Device::GetChannel> ch2 = client.GetChannel();
  ASSERT_EQ(ch1.status(), ZX_OK);
  ASSERT_EQ(ch2.status(), ZX_OK);

  auto prop1 = fidl::WireCall<audio_fidl::StreamConfig>(ch1->channel).GetProperties();
  auto prop2 = fidl::WireCall<audio_fidl::StreamConfig>(ch2->channel).GetProperties();
  ASSERT_OK(prop1.status());
  ASSERT_OK(prop2.status());

  ASSERT_EQ(prop1->properties.plug_detect_capabilities(),
            audio_fidl::wire::PlugDetectCapabilities::kCanAsyncNotify);
  ASSERT_EQ(prop2->properties.plug_detect_capabilities(),
            audio_fidl::wire::PlugDetectCapabilities::kCanAsyncNotify);

  // Watch each channel for initial reply.
  auto state1 = fidl::WireCall<audio_fidl::StreamConfig>(ch1->channel).WatchPlugState();
  auto state2 = fidl::WireCall<audio_fidl::StreamConfig>(ch2->channel).WatchPlugState();
  ASSERT_OK(state1.status());
  ASSERT_OK(state2.status());
  ASSERT_FALSE(state1->plug_state.plugged());
  ASSERT_FALSE(state2->plug_state.plugged());

  // Secondary watches with no reply since there is no change of plug detect state.
  auto f = [](void* arg) -> int {
    fidl::WireResult<audio_fidl::Device::GetChannel>* ch =
        static_cast<fidl::WireResult<audio_fidl::Device::GetChannel>*>(arg);
    fidl::WireCall<audio_fidl::StreamConfig>((*ch)->channel).WatchPlugState();
    return 0;
  };
  thrd_t th1;
  ASSERT_OK(thrd_create_with_name(&th1, f, &ch1, "test-thread-1"));
  thrd_t th2;
  ASSERT_OK(thrd_create_with_name(&th2, f, &ch2, "test-thread-2"));

  // We want the watches to be started before we reset the channels triggering deactivations.
  zx::nanosleep(zx::deadline_after(zx::msec(100)));
  ch1->channel.reset();
  ch2->channel.reset();

  int result = -1;
  thrd_join(th1, &result);
  ASSERT_EQ(result, 0);
  result = -1;
  thrd_join(th2, &result);
  ASSERT_EQ(result, 0);
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, MultipleChannelsPlugDetectNotify) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  // We get 2 channels from the one FIDL channel acquired via FidlClient() using GetChannel.
  fidl::WireResult<audio_fidl::Device::GetChannel> ch1 = client.GetChannel();
  fidl::WireResult<audio_fidl::Device::GetChannel> ch2 = client.GetChannel();
  fidl::WireResult<audio_fidl::Device::GetChannel> ch3 = client.GetChannel();
  ASSERT_EQ(ch1.status(), ZX_OK);
  ASSERT_EQ(ch2.status(), ZX_OK);
  ASSERT_EQ(ch3.status(), ZX_OK);

  auto state1a = fidl::WireCall<audio_fidl::StreamConfig>(ch1->channel).WatchPlugState();
  auto state2a = fidl::WireCall<audio_fidl::StreamConfig>(ch2->channel).WatchPlugState();
  auto state3a = fidl::WireCall<audio_fidl::StreamConfig>(ch3->channel).WatchPlugState();
  ASSERT_OK(state1a.status());
  ASSERT_OK(state2a.status());
  ASSERT_OK(state3a.status());
  ASSERT_FALSE(state1a->plug_state.plugged());
  ASSERT_FALSE(state2a->plug_state.plugged());
  ASSERT_FALSE(state3a->plug_state.plugged());

  server->PostSetPlugState(true, zx::duration(zx::msec(100)));

  auto state1b = fidl::WireCall<audio_fidl::StreamConfig>(ch1->channel).WatchPlugState();
  auto state2b = fidl::WireCall<audio_fidl::StreamConfig>(ch2->channel).WatchPlugState();
  auto state3b = fidl::WireCall<audio_fidl::StreamConfig>(ch3->channel).WatchPlugState();
  ASSERT_OK(state1b.status());
  ASSERT_OK(state2b.status());
  ASSERT_OK(state3b.status());
  ASSERT_TRUE(state1b->plug_state.plugged());
  ASSERT_TRUE(state2b->plug_state.plugged());
  ASSERT_TRUE(state3b->plug_state.plugged());
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, MultipleChannelsGainState) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  // We get 2 channels from the one FIDL channel acquired via FidlClient() using GetChannel.
  fidl::WireResult<audio_fidl::Device::GetChannel> ch1 = client.GetChannel();
  fidl::WireResult<audio_fidl::Device::GetChannel> ch2 = client.GetChannel();
  ASSERT_EQ(ch1.status(), ZX_OK);
  ASSERT_EQ(ch2.status(), ZX_OK);

  auto state1 = fidl::WireCall<audio_fidl::StreamConfig>(ch1->channel).WatchGainState();
  auto state2 = fidl::WireCall<audio_fidl::StreamConfig>(ch2->channel).WatchGainState();
  ASSERT_OK(state1.status());
  ASSERT_OK(state2.status());
  ASSERT_EQ(0.f, state1->gain_state.gain_db());
  ASSERT_EQ(0.f, state2->gain_state.gain_db());
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, MultipleChannelsGainStateNotify) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  // We get 2 channels from the one FIDL channel acquired via FidlClient() using GetChannel.
  fidl::WireResult<audio_fidl::Device::GetChannel> ch1 = client.GetChannel();
  fidl::WireResult<audio_fidl::Device::GetChannel> ch2 = client.GetChannel();
  fidl::WireResult<audio_fidl::Device::GetChannel> ch3 = client.GetChannel();
  ASSERT_EQ(ch1.status(), ZX_OK);
  ASSERT_EQ(ch2.status(), ZX_OK);
  ASSERT_EQ(ch3.status(), ZX_OK);

  auto state1a = fidl::WireCall<audio_fidl::StreamConfig>(ch1->channel).WatchGainState();
  auto state2a = fidl::WireCall<audio_fidl::StreamConfig>(ch2->channel).WatchGainState();
  auto state3a = fidl::WireCall<audio_fidl::StreamConfig>(ch3->channel).WatchGainState();
  ASSERT_OK(state1a.status());
  ASSERT_OK(state2a.status());
  ASSERT_OK(state3a.status());
  ASSERT_EQ(0.f, state1a->gain_state.gain_db());
  ASSERT_EQ(0.f, state2a->gain_state.gain_db());
  ASSERT_EQ(0.f, state3a->gain_state.gain_db());

  auto f = [](void* arg) -> int {
    zx::nanosleep(zx::deadline_after(zx::msec(100)));
    fidl::WireResult<audio_fidl::Device::GetChannel>* ch1 =
        static_cast<fidl::WireResult<audio_fidl::Device::GetChannel>*>(arg);

    fidl::Arena allocator;
    audio_fidl::wire::GainState gain_state(allocator);
    gain_state.set_muted(allocator, false)
        .set_agc_enabled(allocator, false)
        .set_gain_db(allocator, MockSimpleAudio::kTestGain);
    fidl::WireCall<audio_fidl::StreamConfig>((*ch1)->channel).SetGain(std::move(gain_state));

    return 0;
  };
  thrd_t th;
  ASSERT_OK(thrd_create_with_name(&th, f, &ch1, "test-thread"));

  auto state1b = fidl::WireCall<audio_fidl::StreamConfig>(ch1->channel).WatchGainState();
  auto state2b = fidl::WireCall<audio_fidl::StreamConfig>(ch2->channel).WatchGainState();
  auto state3b = fidl::WireCall<audio_fidl::StreamConfig>(ch3->channel).WatchGainState();
  ASSERT_OK(state1b.status());
  ASSERT_OK(state2b.status());
  ASSERT_OK(state3b.status());
  ASSERT_EQ(MockSimpleAudio::kTestGain, state1b->gain_state.gain_db());
  ASSERT_EQ(MockSimpleAudio::kTestGain, state2b->gain_state.gain_db());
  ASSERT_EQ(MockSimpleAudio::kTestGain, state3b->gain_state.gain_db());

  int result = -1;
  thrd_join(th, &result);
  ASSERT_EQ(result, 0);
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, RingBufferTests) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  auto endpoints = fidl::CreateEndpoints<audio_fidl::RingBuffer>();
  ASSERT_OK(endpoints.status_value());
  auto [local, remote] = std::move(endpoints.value());

  fidl::Arena allocator;
  audio_fidl::wire::Format format(allocator);
  format.set_pcm_format(allocator, GetDefaultPcmFormat());

  auto rb = fidl::WireCall<audio_fidl::StreamConfig>(ch->channel)
                .CreateRingBuffer(std::move(format), std::move(remote));
  ASSERT_OK(rb.status());

  constexpr uint32_t kNumberOfPositionNotifications = 5;
  // Buffer is set to hold at least 1 second, with kNumberOfPositionNotifications notifications
  // per ring buffer (i.e. per second) we set the time waiting for the watch below to 200ms+.

  auto vmo = fidl::WireCall<audio_fidl::RingBuffer>(local).GetVmo(MockSimpleAudio::kTestFrameRate,
                                                                  kNumberOfPositionNotifications);
  ASSERT_OK(vmo.status());

  constexpr uint64_t kSomeActiveChannelsMask = 0xc3;
  auto active_channels =
      fidl::WireCall<audio_fidl::RingBuffer>(local).SetActiveChannels(kSomeActiveChannelsMask);
  ASSERT_OK(active_channels.status());
  ASSERT_EQ(active_channels->result.err(), ZX_ERR_NOT_SUPPORTED);

  // Check inspect state.
  {
    ASSERT_NO_FATAL_FAILURES(ReadInspect(server->inspect().DuplicateVmo()));
    auto* simple_audio = hierarchy().GetByPath({"simple_audio_stream"});
    ASSERT_TRUE(simple_audio);
    ASSERT_NO_FATAL_FAILURES(
        CheckProperty(simple_audio->node(), "state", inspect::StringPropertyValue("created")));
    ASSERT_NO_FATAL_FAILURES(
        CheckProperty(simple_audio->node(), "start_time", inspect::IntPropertyValue(0)));
    ASSERT_NO_FATAL_FAILURES(
        CheckProperty(simple_audio->node(), "frames_requested",
                      inspect::UintPropertyValue(MockSimpleAudio::kTestFrameRate)));
  }

  auto start = fidl::WireCall<audio_fidl::RingBuffer>(local).Start();
  ASSERT_OK(start.status());

  // Check updated inspect state.
  {
    ASSERT_NO_FATAL_FAILURES(ReadInspect(server->inspect().DuplicateVmo()));
    auto* simple_audio = hierarchy().GetByPath({"simple_audio_stream"});
    ASSERT_TRUE(simple_audio);
    ASSERT_NO_FATAL_FAILURES(
        CheckProperty(simple_audio->node(), "state", inspect::StringPropertyValue("started")));
    ASSERT_NO_FATAL_FAILURES(
        CheckPropertyNotEqual(simple_audio->node(), "start_time", inspect::IntPropertyValue(0)));
  }

  auto position = fidl::WireCall<audio_fidl::RingBuffer>(local).WatchClockRecoveryPositionInfo();
  ASSERT_EQ(MockSimpleAudio::kTestPositionNotify, position->position_info.position);

  auto stop = fidl::WireCall<audio_fidl::RingBuffer>(local).Stop();
  ASSERT_OK(stop.status());
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, WatchPositionAndCloseRingBufferBeforeReply) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client;
  *client.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  auto endpoints = fidl::CreateEndpoints<audio_fidl::RingBuffer>();
  ASSERT_OK(endpoints.status_value());
  auto [local, remote] = std::move(endpoints.value());

  fidl::Arena allocator;
  audio_fidl::wire::Format format(allocator);
  format.set_pcm_format(allocator, GetDefaultPcmFormat());

  auto rb = fidl::WireCall<audio_fidl::StreamConfig>(ch->channel)
                .CreateRingBuffer(std::move(format), std::move(remote));
  ASSERT_OK(rb.status());

  constexpr uint32_t kNumberOfPositionNotifications = 5;
  // Buffer is set to hold at least 1 second, with kNumberOfPositionNotifications notifications
  // per ring buffer (i.e. per second) the time waiting before getting a position reply is 200ms+.

  auto vmo = fidl::WireCall<audio_fidl::RingBuffer>(local).GetVmo(MockSimpleAudio::kTestFrameRate,
                                                                  kNumberOfPositionNotifications);
  ASSERT_OK(vmo.status());

  auto start = fidl::WireCall<audio_fidl::RingBuffer>(local).Start();
  ASSERT_OK(start.status());

  // Watch position notifications.
  auto f = [](void* arg) -> int {
    auto ch = static_cast<fidl::ClientEnd<audio_fidl::RingBuffer>*>(arg);
    fidl::WireCall<audio_fidl::RingBuffer>(*ch).WatchClockRecoveryPositionInfo();
    return 0;
  };
  thrd_t th;
  ASSERT_OK(thrd_create_with_name(&th, f, &local, "test-thread"));

  // We want the watch to be started before we reset the channel triggering a deactivation.
  zx::nanosleep(zx::deadline_after(zx::msec(100)));
  local.reset();
  ch->channel.reset();

  int result = -1;
  thrd_join(th, &result);
  ASSERT_EQ(result, 0);
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, ClientCloseStreamConfigProtocol) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client_wrap;
  *client_wrap.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client_wrap.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  ch->channel.reset();
  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, ClientCloseRingBufferProtocol) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client_wrap;
  *client_wrap.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client_wrap.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  fidl::WireSyncClient<audio_fidl::StreamConfig> client(std::move(ch->channel));

  auto endpoints = fidl::CreateEndpoints<audio_fidl::RingBuffer>();
  ASSERT_OK(endpoints.status_value());

  fidl::Arena allocator;
  audio_fidl::wire::Format format(allocator);
  format.set_pcm_format(allocator, GetDefaultPcmFormat());

  auto ret = client.CreateRingBuffer(std::move(format), std::move(endpoints->server));
  ASSERT_OK(ret.status());

  endpoints->client.reset();

  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, ClientCloseStreamConfigProtocolWithARingBufferProtocol) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client_wrap;
  *client_wrap.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch = client_wrap.GetChannel();
  ASSERT_EQ(ch.status(), ZX_OK);

  fidl::WireSyncClient<audio_fidl::StreamConfig> client(std::move(ch->channel));

  auto endpoints = fidl::CreateEndpoints<audio_fidl::RingBuffer>();
  ASSERT_OK(endpoints.status_value());

  fidl::Arena allocator;
  audio_fidl::wire::Format format(allocator);
  format.set_pcm_format(allocator, GetDefaultPcmFormat());

  auto ret = client.CreateRingBuffer(std::move(format), std::move(endpoints->server));
  ASSERT_OK(ret.status());

  client.mutable_channel()->reset();

  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

TEST_F(SimpleAudioTest, NonPriviledged) {
  auto server = SimpleAudioStream::Create<MockSimpleAudio>(fake_ddk::kFakeParent);
  ASSERT_NOT_NULL(server);

  fidl::WireSyncClient<audio_fidl::Device> client_wrap;
  *client_wrap.mutable_channel() = std::move(ddk_.FidlClient());
  fidl::WireResult<audio_fidl::Device::GetChannel> ch1 = client_wrap.GetChannel();
  fidl::WireResult<audio_fidl::Device::GetChannel> ch2 = client_wrap.GetChannel();
  fidl::WireResult<audio_fidl::Device::GetChannel> ch3 = client_wrap.GetChannel();
  ASSERT_EQ(ch1.status(), ZX_OK);
  ASSERT_EQ(ch2.status(), ZX_OK);
  ASSERT_EQ(ch3.status(), ZX_OK);

  auto channel1 = zx::unowned_channel(ch1->channel.channel());
  fidl::WireSyncClient<audio_fidl::StreamConfig> client1(std::move(ch1->channel));
  auto endpoints1 = fidl::CreateEndpoints<audio_fidl::RingBuffer>();
  ASSERT_OK(endpoints1.status_value());

  {
    fidl::Arena allocator;
    audio_fidl::wire::Format format(allocator);
    format.set_pcm_format(allocator, GetDefaultPcmFormat());

    auto ret1 = client1.CreateRingBuffer(std::move(format), std::move(endpoints1->server));
    ASSERT_OK(ret1.status());
  }
  fidl::WireSyncClient<audio_fidl::RingBuffer> ringbuffer1(std::move(endpoints1->client));
  auto stop1 = ringbuffer1.Stop();
  ASSERT_OK(stop1.status());  // Priviledged channel.

  auto channel2 = zx::unowned_channel(ch2->channel.channel());
  fidl::WireSyncClient<audio_fidl::StreamConfig> client2(std::move(ch2->channel));
  auto endpoints2 = fidl::CreateEndpoints<audio_fidl::RingBuffer>();
  ASSERT_OK(endpoints2.status_value());

  {
    fidl::Arena allocator;
    audio_fidl::wire::Format format(allocator);
    format.set_pcm_format(allocator, GetDefaultPcmFormat());

    auto ret2 = client2.CreateRingBuffer(std::move(format), std::move(endpoints2->server));
    ASSERT_OK(ret2.status());
  }
  fidl::WireSyncClient<audio_fidl::RingBuffer> ringbuffer2(std::move(endpoints2->client));
  auto stop2 = ringbuffer2.Stop();
  ASSERT_NOT_OK(stop2.status());  // Non-priviledged channel.

  auto channel3 = zx::unowned_channel(ch3->channel.channel());
  fidl::WireSyncClient<audio_fidl::StreamConfig> client3(std::move(ch3->channel));
  auto endpoints3 = fidl::CreateEndpoints<audio_fidl::RingBuffer>();
  ASSERT_OK(endpoints3.status_value());

  {
    fidl::Arena allocator;
    audio_fidl::wire::Format format(allocator);
    format.set_pcm_format(allocator, GetDefaultPcmFormat());

    auto ret3 = client3.CreateRingBuffer(std::move(format), std::move(endpoints3->server));
    ASSERT_OK(ret3.status());
  }
  fidl::WireSyncClient<audio_fidl::RingBuffer> ringbuffer3(std::move(endpoints3->client));
  auto stop3 = ringbuffer3.Stop();
  ASSERT_NOT_OK(stop3.status());  // Non-priviledged channel.

  server->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
  server->DdkRelease();
}

}  // namespace audio

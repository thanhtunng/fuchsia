// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tas5720.h"

#include <fuchsia/hardware/i2c/c/banjo.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/zx/time.h>

#include <algorithm>
#include <memory>

#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>

#include "src/media/audio/drivers/codecs/tas5720/ti_tas5720-bind.h"

namespace audio {

// This codec offers a DAI interface with 2 channel I2S, even though it is a mono amp with the
// channel actually amplified specified via metadata for a particular product.
static const std::vector<uint32_t> kSupportedNumberOfChannels = {2};
static const std::vector<SampleFormat> kSupportedSampleFormats = {SampleFormat::PCM_SIGNED};
static const std::vector<FrameFormat> kSupportedFrameFormats = {FrameFormat::STEREO_LEFT,
                                                                FrameFormat::I2S};
static const std::vector<uint32_t> kSupportedRates = {48'000, 96'000};
static const std::vector<uint8_t> kSupportedBitsPerSlot = {32};
static const std::vector<uint8_t> kSupportedBitsPerSample = {16};
static const audio::DaiSupportedFormats kSupportedDaiFormats = {
    .number_of_channels = kSupportedNumberOfChannels,
    .sample_formats = kSupportedSampleFormats,
    .frame_formats = kSupportedFrameFormats,
    .frame_rates = kSupportedRates,
    .bits_per_slot = kSupportedBitsPerSlot,
    .bits_per_sample = kSupportedBitsPerSample,
};

constexpr float Tas5720::kMaxGain;
constexpr float Tas5720::kMinGain;
// clang-format off
constexpr uint8_t kRegPowerControl        = 0x01;
constexpr uint8_t kRegDigitalControl1     = 0x02;
constexpr uint8_t kRegDigitalControl2     = 0x03;
constexpr uint8_t kRegVolumeControl       = 0x04;
constexpr uint8_t kRegAnalogControl       = 0x06;
constexpr uint8_t kRegDigitalClipper2     = 0x10;
constexpr uint8_t kRegDigitalClipper1     = 0x11;
// clang-format on

zx_status_t Tas5720::SetMuted(bool mute) {
  uint8_t val = 0;
  auto status = ReadReg(kRegDigitalControl2, &val);
  if (status != ZX_OK) {
    return status;
  }
  return WriteReg(kRegDigitalControl2, static_cast<uint8_t>(mute ? (val | 0x10) : (val & ~0x10)));
}

zx_status_t Tas5720::SetSlot(uint8_t slot) {
  uint8_t val = 0;
  auto status = ReadReg(kRegDigitalControl2, &val);
  if (status != ZX_OK) {
    return status;
  }
  return WriteReg(kRegDigitalControl2, static_cast<uint8_t>((val & ~3) | (tdm_slot_)));
}

zx_status_t Tas5720::SetGain(float gain) {
  gain = std::clamp(gain, kMinGain, kMaxGain);
  float digital_gain = gain;
  // For gains lower than 0dB we lower the analog gain first.
  uint8_t analog_setting = 3;  // 26.3dBV.
  if (gain >= 0.f) {
    // Apply the gain directly as digital volume, keep analog_setting at 3.
  } else if (gain >= -2.8f) {
    digital_gain += 2.8f;
    analog_setting = 2;  // 23.5dBV.
  } else if (gain >= -5.6f) {
    digital_gain += 5.6f;
    analog_setting = 1;  // 20.7dBV.
  } else {
    digital_gain += 7.1f;
    analog_setting = 0;  // 19.2dBV.
  }
  constexpr uint8_t kPwmRate = 0x05;  // 16 x lrclk.
  constexpr uint8_t kReserved = 0x01;
  auto status = WriteReg(kRegAnalogControl,
                         static_cast<uint8_t>((kPwmRate << 4) | (analog_setting << 2) | kReserved));
  if (status != ZX_OK) {
    return status;
  }
  // Datasheet: "DVC [Hex Value] = 0xCF + (DVC [dB] / 0.5 [dB] )".
  uint8_t gain_reg = static_cast<uint8_t>(0xCF + digital_gain / .5f);
  return WriteReg(kRegVolumeControl, gain_reg);
}

bool Tas5720::ValidGain(float gain) const { return (gain <= kMaxGain) && (gain >= kMinGain); }

zx_status_t Tas5720::SetRateAndFormat() {
  if (rate_ != 48000 && rate_ != 96000) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  // Use Slot, Left-Justified/I2S. If 96kHz set double rate.
  return WriteReg(kRegDigitalControl1, ((rate_ == 96000) ? 0x08 : 0) | 0x40 | (i2s_ ? 4 : 5));
}

zx_status_t Tas5720::Shutdown() { return Stop(); }

zx_status_t Tas5720::Reinitialize() {
  auto status = Stop();
  if (status != ZX_OK) {
    return status;
  }
  rate_ = kSupportedRates[0];
  status = SetRateAndFormat();
  if (status != ZX_OK) {
    return status;
  }
  constexpr uint8_t kReservedBitsSet = 0x80;
  status =
      WriteReg(kRegDigitalControl2, kReservedBitsSet | (tdm_slot_) | 0x10);  // TDM slot, Muted.
  if (status != ZX_OK) {
    return status;
  }
  constexpr uint8_t kAnalogSetting = 3;  // 26.3dBV.
  constexpr uint8_t kPwmRate = 0x05;     // 16 x lrclk.
  constexpr uint8_t kReserved = 0x01;
  status = WriteReg(kRegAnalogControl,
                    static_cast<uint8_t>((kPwmRate << 4) | (kAnalogSetting << 2) | kReserved));
  if (status != ZX_OK) {
    return status;
  }
  status = WriteReg(kRegDigitalClipper2, 0xFF);  // Disabled.
  if (status != ZX_OK) {
    return status;
  }
  status = WriteReg(kRegDigitalClipper1, 0xFC);  // Disabled.
  if (status != ZX_OK) {
    return status;
  }
  status = Start();
  if (status != ZX_OK) {
    return status;
  }
  constexpr float kDefaultGainDb = -30.f;
  GainState gain_state = {.gain = kDefaultGainDb, .muted = true};
  SetGainState(std::move(gain_state));
  return ZX_OK;
}

zx::status<DriverIds> Tas5720::Initialize() {
  size_t actual = 0;
  auto status = device_get_metadata(parent(), DEVICE_METADATA_PRIVATE, &instance_count_,
                                    sizeof(instance_count_), &actual);
  if (status != ZX_OK || sizeof(instance_count_) != actual) {
    zxlogf(ERROR, "device_get_metadata failed %d", status);
    return zx::error(status);
  }

  status = Reset();
  if (status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(DriverIds{
      .vendor_id = PDEV_VID_TI,
      .device_id = PDEV_DID_TI_TAS5720,
      .instance_count = instance_count_,
  });
}

zx_status_t Tas5720::Reset() {
  auto status = Shutdown();
  if (status != ZX_OK) {
    return status;
  }
  uint8_t r;
  status = ReadReg(kRegPowerControl, &r);
  if (status != ZX_OK) {
    return status;
  }
  r = static_cast<uint8_t>(r | 0x01);  // SPK_SDZ exit shutdown.
  status = WriteReg(kRegPowerControl, r);
  if (status != ZX_OK) {
    return status;
  }
  return Reinitialize();
}

Info Tas5720::GetInfo() {
  return {
      .unique_id = "",
      .manufacturer = "Texas Instruments",
      .product_name = "TAS5720",
  };
}

zx_status_t Tas5720::Stop() {
  uint8_t r;
  auto status = ReadReg(kRegPowerControl, &r);
  if (status != ZX_OK) {
    return status;
  }
  r = static_cast<uint8_t>(r & ~(0x01));  // SPK_SDZ enter shutdown.
  return WriteReg(kRegPowerControl, r);
}

zx_status_t Tas5720::Start() {
  uint8_t r;
  auto status = ReadReg(kRegPowerControl, &r);
  if (status != ZX_OK) {
    return status;
  }
  r = static_cast<uint8_t>(r | 0x01);  // SPK_SDZ exit shutdown.
  return WriteReg(kRegPowerControl, r);
}

bool Tas5720::IsBridgeable() { return false; }

void Tas5720::SetBridgedMode(bool enable_bridged_mode) {
  if (enable_bridged_mode) {
    zxlogf(INFO, "tas5720: bridged mode note supported");
  }
}
DaiSupportedFormats Tas5720::GetDaiFormats() { return kSupportedDaiFormats; }

zx::status<CodecFormatInfo> Tas5720::SetDaiFormat(const DaiFormat& format) {
  ZX_ASSERT(format.channels_to_use_bitmask == 1 ||
            format.channels_to_use_bitmask == 2);  // Mono codec, 2 channel TDM.
  tdm_slot_ = static_cast<uint8_t>(__builtin_ctzl(format.channels_to_use_bitmask));
  i2s_ = format.frame_format == FrameFormat::I2S;
  auto status = SetSlot(tdm_slot_);
  if (status != ZX_OK) {
    return zx::error(status);
  }
  rate_ = format.frame_rate;
  status = SetRateAndFormat();
  if (status != ZX_OK) {
    return zx::error(status);
  }
  CodecFormatInfo state = {};
  // Turn on delay is tACTIVE (25ms) + tVRAMP (ramp time from -100dB to 0dB).
  if (rate_ >= 96'000) {
    state.set_turn_on_delay(zx::msec(25).get() + zx::msec(16.7).get());
  } else if (rate_ >= 88'200) {
    state.set_turn_on_delay(zx::msec(25).get() + zx::msec(18.1).get());
  } else if (rate_ >= 48'000) {
    state.set_turn_on_delay(zx::msec(25).get() + zx::msec(33.3).get());
  } else {
    state.set_turn_on_delay(zx::msec(25).get() + zx::msec(36.3).get());
  }
  return zx::ok(std::move(state));
}

GainFormat Tas5720::GetGainFormat() {
  return {
      .min_gain = kMinGain,
      .max_gain = kMaxGain,
      .gain_step = kGainStep,
      .can_mute = true,
      .can_agc = false,
  };
}

GainState Tas5720::GetGainState() { return gain_state_; }

void Tas5720::SetGainState(GainState gain_state) {
  auto status = SetGain(gain_state.gain);
  if (status != ZX_OK) {
    zxlogf(ERROR, "tas5720: Could not set gain %d", status);
  }
  status = SetMuted(gain_state.muted);
  if (status != ZX_OK) {
    zxlogf(ERROR, "tas5720: Could not set mute state %d", status);
  }
  if (gain_state.agc_enabled) {
    zxlogf(ERROR, "tas5720: AGC enable not supported");
    gain_state.agc_enabled = false;
  }
  gain_state_ = gain_state;
}

zx_status_t Tas5720::WriteReg(uint8_t reg, uint8_t value) {
  uint8_t write_buffer[2];
  write_buffer[0] = reg;
  write_buffer[1] = value;
//#define TRACE_I2C
#ifdef TRACE_I2C
  printf("Writing to instance/slot %u/%u register 0x%02X to value 0x%02X\n", instance_count_,
         tdm_slot_, reg, value);
  auto status = i2c_.WriteSync(write_buffer, countof(write_buffer));
  if (status != ZX_OK) {
    printf("Could not I2C write %d\n", status);
    return status;
  }
  return ZX_OK;
#else
  constexpr uint8_t kNumberOfRetries = 2;
  constexpr zx::duration kRetryDelay = zx::msec(1);
  auto ret =
      i2c_.WriteSyncRetries(write_buffer, countof(write_buffer), kNumberOfRetries, kRetryDelay);
  if (ret.status != ZX_OK) {
    zxlogf(ERROR, "tas5720: I2C write reg 0x%02X error %d, %d retries", reg, ret.status,
           ret.retries);
  }
  return ret.status;
#endif
}

zx_status_t Tas5720::ReadReg(uint8_t reg, uint8_t* value) {
  constexpr uint8_t kNumberOfRetries = 2;
  constexpr zx::duration kRetryDelay = zx::msec(1);
  auto ret = i2c_.WriteReadSyncRetries(&reg, 1, value, 1, kNumberOfRetries, kRetryDelay);
  if (ret.status != ZX_OK) {
    zxlogf(ERROR, "tas5720: I2C read reg 0x%02X error %d, %d retries", reg, ret.status,
           ret.retries);
  }
#ifdef TRACE_I2C
  printf("Read register 0x%02X, value %02X\n", reg, *value);
#endif
  return ret.status;
}

zx_status_t tas5720_bind(void* ctx, zx_device_t* parent) {
  ddk::I2cChannel i2c(parent, "i2c");
  if (!i2c.is_valid()) {
    zxlogf(ERROR, "tas5720: Could not get i2c protocol");
    return ZX_ERR_NOT_SUPPORTED;
  }

  auto dev = SimpleCodecServer::Create<Tas5720>(parent, i2c);

  // devmgr is now in charge of the memory for dev.
  dev.release();
  return ZX_OK;
}

static zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = tas5720_bind;
  return ops;
}();
}  // namespace audio

// clang-format off
ZIRCON_DRIVER(ti_tas5720, audio::driver_ops, "zircon", "0.1");

// clang-format on

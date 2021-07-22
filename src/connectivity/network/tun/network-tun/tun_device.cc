// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tun_device.h"

#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/syslog/global.h>
#include <zircon/status.h>

#include <fbl/auto_lock.h>

#include "buffer.h"

namespace network {
namespace tun {

namespace {
template <typename F>
void WithWireState(F fn, InternalState& state) {
  fuchsia_net_tun::wire::InternalState::Frame_ frame;
  fuchsia_net_tun::wire::InternalState wire_state(
      fidl::ObjectView<fuchsia_net_tun::wire::InternalState::Frame_>::FromExternal(&frame));
  wire_state.set_has_session(fidl::ObjectView<bool>::FromExternal(&state.has_session));

  fuchsia_net_tun::wire::MacState::Frame_ frame_mac;
  fuchsia_net_tun::wire::MacState wire_mac(
      fidl::ObjectView<fuchsia_net_tun::wire::MacState::Frame_>::FromExternal(&frame_mac));
  fidl::VectorView<fuchsia_net::wire::MacAddress> multicast_filters;
  if (state.mac.has_value()) {
    MacState& mac = state.mac.value();
    wire_mac.set_mode(
        fidl::ObjectView<fuchsia_hardware_network::wire::MacFilterMode>::FromExternal(&mac.mode));
    multicast_filters =
        fidl::VectorView<fuchsia_net::wire::MacAddress>::FromExternal(mac.multicast_filters);
    wire_mac.set_multicast_filters(
        fidl::ObjectView<fidl::VectorView<fuchsia_net::wire::MacAddress>>::FromExternal(
            &multicast_filters));
    wire_state.set_mac(fidl::ObjectView<fuchsia_net_tun::wire::MacState>::FromExternal(&wire_mac));
  }

  fn(std::move(wire_state));
}
}  // namespace

TunDevice::TunDevice(fit::callback<void(TunDevice*)> teardown, DeviceConfig config)
    : teardown_callback_(std::move(teardown)),
      config_(std::move(config)),
      loop_(&kAsyncLoopConfigNoAttachToCurrentThread) {}

zx::status<std::unique_ptr<TunDevice>> TunDevice::Create(
    fit::callback<void(TunDevice*)> teardown, const fuchsia_net_tun::wire::DeviceConfig& config) {
  fbl::AllocChecker ac;
  std::unique_ptr<TunDevice> tun(new (&ac) TunDevice(std::move(teardown), DeviceConfig(config)));
  if (!ac.check()) {
    return zx::error(ZX_ERR_NO_MEMORY);
  }

  if (zx_status_t status = zx::eventpair::create(0, &tun->signals_peer_, &tun->signals_self_);
      status != ZX_OK) {
    FX_LOGF(ERROR, "tun", "TunDevice::Init failed to create eventpair %s",
            zx_status_get_string(status));
    return zx::error(status);
  }

  zx::status device = DeviceAdapter::Create(tun->loop_.dispatcher(), tun.get());
  if (device.is_error()) {
    FX_LOGF(ERROR, "tun", "TunDevice::Init device init failed with %s", device.status_string());
    return device.take_error();
  }
  tun->device_ = std::move(device.value());

  thrd_t thread;
  if (zx_status_t status = tun->loop_.StartThread("tun-device", &thread); status != ZX_OK) {
    return zx::error(status);
  }
  tun->loop_thread_ = thread;

  return zx::ok(std::move(tun));
}

TunDevice::~TunDevice() {
  if (loop_thread_.has_value()) {
    // not allowed to destroy a tun device on the loop thread, will cause deadlock
    ZX_ASSERT(loop_thread_.value() != thrd_current());
  }
  // make sure that device is torn down:
  if (device_) {
    device_->TeardownSync();
  }
  loop_.Shutdown();
  FX_VLOG(1, "tun", "TunDevice destroyed");
}

void TunDevice::Bind(fidl::ServerEnd<fuchsia_net_tun::Device2> req) {
  legacy_binding_ = fidl::BindServer<LegacyDevice>(
      loop_.dispatcher(), std::move(req), this,
      [](LegacyDevice* impl, fidl::UnbindInfo, fidl::ServerEnd<fuchsia_net_tun::Device2>) {
        static_cast<TunDevice*>(impl)->Teardown();
      });
}

void TunDevice::Bind(fidl::ServerEnd<fuchsia_net_tun::Device> req) {
  binding_ = fidl::BindServer<NewDevice>(
      loop_.dispatcher(), std::move(req), this,
      [](NewDevice* impl, fidl::UnbindInfo, fidl::ServerEnd<fuchsia_net_tun::Device>) {
        static_cast<TunDevice*>(impl)->Teardown();
      });
}

void TunDevice::Teardown() {
  if (teardown_callback_) {
    teardown_callback_(this);
  }
}

template <typename F, typename C>
bool TunDevice::WriteWith(F fn, C& callback) {
  zx::status avail = fn();
  switch (zx_status_t status = avail.status_value(); status) {
    case ZX_OK:
      if (avail.value() == 0) {
        // Clear the writable signal if no more buffers are available afterwards.
        signals_self_.signal_peer(uint32_t(fuchsia_net_tun::wire::Signals::kWritable), 0);
      }
      callback(ZX_OK);
      return true;
    case ZX_ERR_SHOULD_WAIT:
      if (IsBlocking()) {
        return false;
      }
      __FALLTHROUGH;
    default:
      callback(status);
      return true;
  }
}

bool TunDevice::RunWriteFrame() {
  while (!pending_write_frame_.empty()) {
    auto& pending = pending_write_frame_.front();
    bool handled = WriteWith(
        [this, &pending]() -> zx::status<size_t> {
          std::unique_ptr<Port>& port = ports_[pending.port_id];
          if (!port) {
            return zx::error(ZX_ERR_NOT_FOUND);
          }
          return device_->WriteRxFrame(port->adapter(), pending.frame_type, pending.data,
                                       pending.meta);
        },
        pending.callback);
    if (!handled) {
      return false;
    }
    pending_write_frame_.pop();
  }
  return true;
}

void TunDevice::RunReadFrame() {
  while (!pending_read_frame_.empty()) {
    bool success = device_->TryGetTxBuffer([this](TxBuffer& buff, size_t avail) {
      uint8_t port_id = buff.port_id();
      if (!ports_[port_id] || !ports_[port_id]->adapter().online()) {
        return ZX_ERR_UNAVAILABLE;
      }
      std::vector<uint8_t> data;
      zx_status_t status = buff.Read(data);
      if (status != ZX_OK) {
        FX_LOGF(ERROR, "tun", "Failed to read from tx buffer: %s", zx_status_get_string(status));
        // The error reported here is relayed back to clients as an errored tx frame. There's a
        // contract about specific meanings of errors returned in a tx frame through the netdevice
        // banjo API and it might not match the semantics of the buffer API that generated this
        // error. To avoid the possible impedance mismatch, return a fixed error.
        return ZX_ERR_INTERNAL;
      }
      if (data.empty()) {
        FX_LOG(WARNING, "tun", "Ignoring empty tx buffer");
        return ZX_OK;
      }
      fuchsia_net_tun::wire::Frame::Frame_ fidl_frame;
      fuchsia_net_tun::wire::Frame frame(
          fidl::ObjectView<fuchsia_net_tun::wire::Frame::Frame_>::FromExternal(&fidl_frame));
      fidl::VectorView data_view = fidl::VectorView<uint8_t>::FromExternal(data);
      frame.set_data(fidl::ObjectView<fidl::VectorView<uint8_t>>::FromExternal(&data_view));
      netdev::wire::FrameType frame_type = buff.frame_type();
      frame.set_frame_type(fidl::ObjectView<netdev::wire::FrameType>::FromExternal(&frame_type));
      frame.set_port(fidl::ObjectView<uint8_t>::FromExternal(&port_id));
      std::optional meta = buff.TakeMetadata();
      if (meta.has_value()) {
        frame.set_meta(
            fidl::ObjectView<fuchsia_net_tun::wire::FrameMetadata>::FromExternal(&meta.value()));
      }
      pending_read_frame_.front()(zx::ok(frame));
      pending_read_frame_.pop();
      if (avail == 0) {
        // clear Signals::READABLE if we don't have any more tx buffers.
        signals_self_.signal_peer(uint32_t(fuchsia_net_tun::wire::Signals::kReadable), 0);
      }
      return ZX_OK;
    });
    if (!success) {
      if (IsBlocking()) {
        return;
      }
      pending_read_frame_.front()(zx::error(ZX_ERR_SHOULD_WAIT));
      pending_read_frame_.pop();
    }
  }
}

InternalState TunDevice::Port::State() const {
  InternalState state = {
      .has_session = adapter_->has_sessions(),
  };

  const std::unique_ptr<MacAdapter>& mac = adapter_->mac();
  if (mac) {
    state.mac = mac->GetMacState();
  }
  return state;
}

void TunDevice::Port::RunStateChange() {
  if (!pending_watch_state_.has_value()) {
    return;
  }

  InternalState state = State();
  // only continue if any changes actually occurred compared to the last observed state
  if (last_state_.has_value() && last_state_.value() == state) {
    return;
  }

  WatchStateCompleter::Async completer = std::exchange(pending_watch_state_, std::nullopt).value();

  WithWireState(
      [&completer](fuchsia_net_tun::wire::InternalState state) {
        completer.Reply(std::move(state));
      },
      state);

  // store the last informed state through WatchState
  last_state_ = std::move(state);
}

void TunDevice::Port::PostRunStateChange() {
  // Skip any spurious calls that might happen before initialization.
  if (!adapter_) {
    return;
  }
  async::PostTask(parent_->loop_.dispatcher(), [parent = parent_, port_id = adapter_->id()]() {
    // Port could be destroyed between callback and dispatch.
    const std::unique_ptr<Port>& port = parent->ports_[port_id];
    if (port) {
      port->RunStateChange();
    }
  });
}

void TunDevice::WriteFrame(fuchsia_net_tun::wire::Frame frame, WriteFrameCallback callback) {
  if (pending_write_frame_.size() >= kMaxPendingOps) {
    callback(ZX_ERR_NO_RESOURCES);
    return;
  }

  if (!frame.has_frame_type()) {
    callback(ZX_ERR_INVALID_ARGS);
    return;
  }
  fuchsia_hardware_network::wire::FrameType& frame_type = frame.frame_type();
  if (!frame.has_data() || frame.data().empty()) {
    callback(ZX_ERR_INVALID_ARGS);
    return;
  }
  fidl::VectorView<uint8_t>& frame_data = frame.data();
  if (!frame.has_port() || frame.port() >= fuchsia_hardware_network::wire::kMaxPorts) {
    callback(ZX_ERR_INVALID_ARGS);
    return;
  }
  uint8_t& port_id = frame.port();

  bool ready = RunWriteFrame();
  if (ready) {
    std::optional<fuchsia_net_tun::wire::FrameMetadata> meta;
    if (frame.has_meta()) {
      meta = frame.meta();
    }
    bool handled = WriteWith(
        [this, &frame_type, &frame_data, &port_id, &meta]() -> zx::status<size_t> {
          std::unique_ptr<Port>& port = ports_[port_id];
          if (!port) {
            return zx::error(ZX_ERR_NOT_FOUND);
          }
          return device_->WriteRxFrame(port->adapter(), frame_type, frame_data, meta);
        },
        callback);
    if (handled) {
      return;
    }
  }
  pending_write_frame_.emplace(frame, std::move(callback));
}

template <typename F>
void TunDevice::WriteFrameGeneric(typename F::WriteFrameRequestView& request,
                                  typename F::WriteFrameCompleter::Sync& completer) {
  WriteFrame(request->frame, [completer = completer.ToAsync()](zx_status_t status) mutable {
    if (status != ZX_OK) {
      completer.ReplyError(status);
    } else {
      completer.ReplySuccess();
    }
  });
}

void TunDevice::WriteFrame(LegacyDevice::WriteFrameRequestView request,
                           LegacyDevice::WriteFrameCompleter::Sync& completer) {
  WriteFrameGeneric<LegacyDevice>(request, completer);
}

void TunDevice::WriteFrame(NewDevice::WriteFrameRequestView request,
                           NewDevice::WriteFrameCompleter::Sync& completer) {
  WriteFrameGeneric<NewDevice>(request, completer);
}

void TunDevice::ReadFrame(ReadFrameCallback callback) {
  if (pending_read_frame_.size() >= kMaxPendingOps) {
    callback(zx::error(ZX_ERR_NO_RESOURCES));
    return;
  }
  pending_read_frame_.push(std::move(callback));
  RunReadFrame();
}

template <typename F>
void TunDevice::ReadFrameGeneric(typename F::ReadFrameRequestView& request,
                                 typename F::ReadFrameCompleter::Sync& completer) {
  ReadFrame(
      [completer = completer.ToAsync()](zx::status<fuchsia_net_tun::wire::Frame> response) mutable {
        if (response.is_error()) {
          completer.ReplyError(response.status_value());
        } else {
          completer.ReplySuccess(std::move(*response));
        }
      });
}

void TunDevice::ReadFrame(LegacyDevice::ReadFrameRequestView request,
                          LegacyDevice::ReadFrameCompleter::Sync& completer) {
  ReadFrameGeneric<LegacyDevice>(request, completer);
}

void TunDevice::ReadFrame(NewDevice::ReadFrameRequestView request,
                          NewDevice::ReadFrameCompleter::Sync& completer) {
  ReadFrameGeneric<NewDevice>(request, completer);
}

template <typename F>
void TunDevice::GetSignalsGeneric(typename F::GetSignalsRequestView& request,
                                  typename F::GetSignalsCompleter::Sync& completer) {
  zx::eventpair dup;
  signals_peer_.duplicate(ZX_RIGHTS_BASIC, &dup);
  completer.Reply(std::move(dup));
}

void TunDevice::GetSignals(LegacyDevice::GetSignalsRequestView request,
                           LegacyDevice::GetSignalsCompleter::Sync& completer) {
  GetSignalsGeneric<LegacyDevice>(request, completer);
}

void TunDevice::GetSignals(NewDevice::GetSignalsRequestView request,
                           NewDevice::GetSignalsCompleter::Sync& completer) {
  GetSignalsGeneric<NewDevice>(request, completer);
}

template <typename F>
void TunDevice::AddPortGeneric(typename F::AddPortRequestView& request) {
  zx_status_t status = [&request, this]() {
    std::optional maybe_port_config = DevicePortConfig::Create(request->config);

    if (!maybe_port_config.has_value()) {
      return ZX_ERR_INVALID_ARGS;
    }
    DevicePortConfig& port_config = maybe_port_config.value();
    std::unique_ptr<Port>& port_slot = ports_[port_config.port_id];
    if (port_slot) {
      return ZX_ERR_ALREADY_EXISTS;
    }

    zx::status maybe_port = Port::Create(this, port_config);
    if (maybe_port.is_error()) {
      return maybe_port.status_value();
    }
    port_slot = std::move(maybe_port.value());
    port_slot->Bind(std::move(request->port));
    return ZX_OK;
  }();
  if (status != ZX_OK) {
    request->port.Close(status);
  }
}

void TunDevice::AddPort(NewDevice::AddPortRequestView request,
                        NewDevice::AddPortCompleter::Sync& _completer) {
  AddPortGeneric<NewDevice>(request);
}

void TunDevice::AddPort(LegacyDevice::AddPortRequestView request,
                        LegacyDevice::AddPortCompleter::Sync& _completer) {
  AddPortGeneric<LegacyDevice>(request);
}

template <typename F>
void TunDevice::GetDeviceGeneric(typename F::GetDeviceRequestView& request) {
  zx_status_t status = device_->Bind(std::move(request->device));
  if (status != ZX_OK) {
    FX_LOGF(ERROR, "tun", "Failed to bind to network device: %s", zx_status_get_string(status));
  }
}

void TunDevice::GetDevice(NewDevice::GetDeviceRequestView request,
                          NewDevice::GetDeviceCompleter::Sync& _completer) {
  GetDeviceGeneric<NewDevice>(request);
}

void TunDevice::GetDevice(LegacyDevice::GetDeviceRequestView request,
                          LegacyDevice::GetDeviceCompleter::Sync& _completer) {
  GetDeviceGeneric<LegacyDevice>(request);
}

void TunDevice::OnTxAvail(DeviceAdapter* device) {
  signals_self_.signal_peer(0, uint32_t(fuchsia_net_tun::wire::Signals::kReadable));
  async::PostTask(loop_.dispatcher(), [this]() { RunReadFrame(); });
}

void TunDevice::OnRxAvail(DeviceAdapter* device) {
  signals_self_.signal_peer(0, uint32_t(fuchsia_net_tun::wire::Signals::kWritable));
  async::PostTask(loop_.dispatcher(), [this]() { RunWriteFrame(); });
}

zx::status<std::unique_ptr<TunDevice::Port>> TunDevice::Port::Create(
    TunDevice* parent, const DevicePortConfig& config) {
  std::unique_ptr<Port> port(new Port(parent));
  std::unique_ptr<MacAdapter> mac;
  if (config.mac.has_value()) {
    zx::status status = MacAdapter::Create(port.get(), config.mac.value(), false);
    if (status.is_error()) {
      return status.take_error();
    }
    mac = std::move(*status);
  }
  port->adapter_ = std::make_unique<PortAdapter>(port.get(), config, std::move(mac));
  parent->device_->AddPort(port->adapter());
  port->SetOnline(config.online);
  return zx::ok(std::move(port));
}

void TunDevice::Port::OnHasSessionsChanged(PortAdapter& port) { PostRunStateChange(); }

void TunDevice::Port::OnPortStatusChanged(PortAdapter& port, const port_status_t& new_status) {
  parent_->device_->OnPortStatusChanged(port.id(), new_status);
}

void TunDevice::Port::OnPortDestroyed(PortAdapter& port) {
  TunDevice& parent = *parent_;
  async::PostTask(parent.loop_.dispatcher(),
                  [&parent, port_id = adapter_->id()]() { parent.ports_[port_id] = nullptr; });
}

void TunDevice::Port::OnMacStateChanged(MacAdapter* adapter) { PostRunStateChange(); }

void TunDevice::Port::GetState(GetStateRequestView request, GetStateCompleter::Sync& completer) {
  InternalState state = State();
  WithWireState(
      [&completer](fuchsia_net_tun::wire::InternalState state) {
        completer.Reply(std::move(state));
      },
      state);
}

void TunDevice::Port::WatchState(WatchStateRequestView request,
                                 WatchStateCompleter::Sync& completer) {
  if (pending_watch_state_) {
    // this is a programming error, we enforce that clients don't do this by closing their channel.
    completer.Close(ZX_ERR_INTERNAL);
    return;
  }
  pending_watch_state_ = completer.ToAsync();
  RunStateChange();
}

void TunDevice::Port::SetOnline(SetOnlineRequestView request, SetOnlineCompleter::Sync& completer) {
  SetOnline(request->online);
  completer.Reply();
}

void TunDevice::Port::SetOnline(bool online) {
  if (!adapter_->SetOnline(online) || online) {
    return;
  }
  // If we just went offline, we may need to complete all pending writes.
  parent_->RunWriteFrame();
  // Discard pending tx frames for all offline ports.
  auto& ports = parent_->ports_;
  parent_->device_->RetainTxBuffers([&ports](TxBuffer& buffer) {
    if (!ports[buffer.port_id()] || !ports[buffer.port_id()]->adapter().online()) {
      return ZX_ERR_UNAVAILABLE;
    }
    return ZX_OK;
  });
}

void TunDevice::Port::Bind(fidl::ServerEnd<fuchsia_net_tun::Port> req) {
  binding_ = fidl::BindServer(parent_->loop_.dispatcher(), std::move(req), this,
                              [parent = parent_, id = adapter().id()](
                                  Port*, fidl::UnbindInfo, fidl::ServerEnd<fuchsia_net_tun::Port>) {
                                parent->device_->RemovePort(id);
                              });
}

}  // namespace tun
}  // namespace network

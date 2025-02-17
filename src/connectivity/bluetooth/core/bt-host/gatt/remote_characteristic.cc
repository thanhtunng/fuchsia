// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remote_characteristic.h"

#include <zircon/assert.h>

#include "client.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/run_or_post.h"
#include "src/connectivity/bluetooth/core/bt-host/common/slab_allocator.h"

namespace bt::gatt {

namespace {

void ReportNotifyStatus(att::Status status, IdType id,
                        RemoteCharacteristic::NotifyStatusCallback callback,
                        async_dispatcher_t* dispatcher) {
  RunOrPost([status, id, cb = std::move(callback)] { cb(status, id); }, dispatcher);
}

void NotifyValue(const ByteBuffer& value, bool maybe_truncated,
                 RemoteCharacteristic::ValueCallback callback, async_dispatcher_t* dispatcher) {
  if (!dispatcher) {
    callback(value, maybe_truncated);
    return;
  }

  auto buffer = NewSlabBuffer(value.size());
  if (buffer) {
    value.Copy(buffer.get());
    async::PostTask(dispatcher, [callback = std::move(callback), val = std::move(buffer),
                                 maybe_truncated] { callback(*val, maybe_truncated); });
  } else {
    bt_log(DEBUG, "gatt", "out of memory!");
  }
}

}  // namespace

RemoteCharacteristic::PendingNotifyRequest::PendingNotifyRequest(async_dispatcher_t* d,
                                                                 ValueCallback value_cb,
                                                                 NotifyStatusCallback status_cb)
    : dispatcher(d), value_callback(std::move(value_cb)), status_callback(std::move(status_cb)) {
  ZX_DEBUG_ASSERT(value_callback);
  ZX_DEBUG_ASSERT(status_callback);
}

RemoteCharacteristic::NotifyHandler::NotifyHandler(async_dispatcher_t* d, ValueCallback cb)
    : dispatcher(d), callback(std::move(cb)) {
  ZX_DEBUG_ASSERT(callback);
}

RemoteCharacteristic::RemoteCharacteristic(fxl::WeakPtr<Client> client,
                                           const CharacteristicData& info)
    : info_(info),
      discovery_error_(false),
      shut_down_(false),
      ccc_handle_(att::kInvalidHandle),
      ext_prop_handle_(att::kInvalidHandle),
      next_notify_handler_id_(1u),
      client_(client),
      weak_ptr_factory_(this) {
  ZX_DEBUG_ASSERT(client_);
}

RemoteCharacteristic::RemoteCharacteristic(RemoteCharacteristic&& other)
    : info_(other.info_),
      discovery_error_(other.discovery_error_),
      shut_down_(other.shut_down_.load()),
      ccc_handle_(other.ccc_handle_),
      ext_prop_handle_(other.ext_prop_handle_),
      next_notify_handler_id_(other.next_notify_handler_id_),
      client_(other.client_),
      weak_ptr_factory_(this) {
  other.weak_ptr_factory_.InvalidateWeakPtrs();
}

void RemoteCharacteristic::ShutDown() {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());

  // Make sure that all weak pointers are invalidated on the GATT thread.
  weak_ptr_factory_.InvalidateWeakPtrs();
  shut_down_ = true;

  ResolvePendingNotifyRequests(att::Status(HostError::kFailed));

  // Clear the CCC if we have enabled notifications.
  // TODO(armansito): Don't write to the descriptor if ShutDown() was called
  // as a result of a "Service Changed" indication.
  if (!notify_handlers_.empty()) {
    notify_handlers_.clear();
    DisableNotificationsInternal();
  }
}

void RemoteCharacteristic::UpdateDataWithExtendedProperties(ExtendedProperties ext_props) {
  // |CharacteristicData| is an immutable snapshot into the data associated with this
  // Characteristic. Update |info_| with the most recent snapshot - the only new member is the
  // recently read |ext_props|.
  info_ =
      CharacteristicData(info_.properties, ext_props, info_.handle, info_.value_handle, info_.type);
}

void RemoteCharacteristic::DiscoverDescriptors(att::Handle range_end,
                                               att::StatusCallback callback) {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  ZX_DEBUG_ASSERT(client_);
  ZX_DEBUG_ASSERT(callback);
  ZX_DEBUG_ASSERT(!shut_down_);
  ZX_DEBUG_ASSERT(range_end >= info().value_handle);

  discovery_error_ = false;
  descriptors_.clear();

  if (info().value_handle == range_end) {
    callback(att::Status());
    return;
  }

  auto self = weak_ptr_factory_.GetWeakPtr();
  auto desc_cb = [self](const DescriptorData& desc) {
    if (!self)
      return;

    ZX_DEBUG_ASSERT(self->thread_checker_.is_thread_valid());
    if (self->discovery_error_)
      return;

    if (desc.type == types::kClientCharacteristicConfig) {
      if (self->ccc_handle_ != att::kInvalidHandle) {
        bt_log(DEBUG, "gatt", "characteristic has more than one CCC descriptor!");
        self->discovery_error_ = true;
        return;
      }
      self->ccc_handle_ = desc.handle;
    } else if (desc.type == types::kCharacteristicExtProperties) {
      if (self->ext_prop_handle_ != att::kInvalidHandle) {
        bt_log(DEBUG, "gatt", "characteristic has more than one Extended Prop descriptor!");
        self->discovery_error_ = true;
        return;
      }

      // If the characteristic properties has the ExtendedProperties bit set, then
      // update the handle.
      if (self->properties() & Property::kExtendedProperties) {
        self->ext_prop_handle_ = desc.handle;
      } else {
        bt_log(DEBUG, "gatt", "characteristic extended properties not set");
      }
    }

    // As descriptors must be strictly increasing, this emplace should always succeed
    auto [_unused, success] = self->descriptors_.try_emplace(DescriptorHandle(desc.handle), desc);
    ZX_DEBUG_ASSERT(success);
  };

  auto status_cb = [self, cb = std::move(callback)](att::Status status) mutable {
    if (!self) {
      cb(att::Status(HostError::kFailed));
      return;
    }

    ZX_DEBUG_ASSERT(self->thread_checker_.is_thread_valid());

    if (self->discovery_error_) {
      status = att::Status(HostError::kFailed);
    }

    if (!status) {
      self->descriptors_.clear();
      cb(status);
      return;
    }

    // If the characteristic contains the ExtendedProperties descriptor, perform a Read operation
    // to get the extended properties before notifying the callback.
    if (self->ext_prop_handle_ != att::kInvalidHandle) {
      auto read_cb = [self, cb = std::move(cb)](att::Status status, const ByteBuffer& data,
                                                bool /*maybe_truncated*/) {
        if (!status) {
          cb(status);
          return;
        }

        // The ExtendedProperties descriptor value is a |uint16_t| representing the
        // ExtendedProperties bitfield. If the retrieved |data| is malformed, respond with an error
        // and return early.
        if (data.size() != sizeof(uint16_t)) {
          cb(att::Status(HostError::kPacketMalformed));
          return;
        }

        auto ext_props = le16toh(data.As<uint16_t>());
        self->UpdateDataWithExtendedProperties(ext_props);

        cb(status);
      };

      self->client_->ReadRequest(self->ext_prop_handle_, std::move(read_cb));
      return;
    }

    cb(status);
  };

  client_->DiscoverDescriptors(info().value_handle + 1, range_end, std::move(desc_cb),
                               std::move(status_cb));
}

void RemoteCharacteristic::EnableNotifications(ValueCallback value_callback,
                                               NotifyStatusCallback status_callback,
                                               async_dispatcher_t* dispatcher) {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  ZX_DEBUG_ASSERT(client_);
  ZX_DEBUG_ASSERT(value_callback);
  ZX_DEBUG_ASSERT(status_callback);
  ZX_DEBUG_ASSERT(!shut_down_);

  if (!(info().properties & (Property::kNotify | Property::kIndicate))) {
    bt_log(DEBUG, "gatt", "characteristic does not support notifications");
    ReportNotifyStatus(att::Status(HostError::kNotSupported), kInvalidId,
                       std::move(status_callback), dispatcher);
    return;
  }

  // If notifications are already enabled then succeed right away.
  if (!notify_handlers_.empty()) {
    ZX_DEBUG_ASSERT(pending_notify_reqs_.empty());

    IdType id = next_notify_handler_id_++;
    notify_handlers_[id] = NotifyHandler(dispatcher, std::move(value_callback));
    ReportNotifyStatus(att::Status(), id, std::move(status_callback), dispatcher);
    return;
  }

  pending_notify_reqs_.emplace(dispatcher, std::move(value_callback), std::move(status_callback));

  // If there are other pending requests to enable notifications then we'll wait
  // until the descriptor write completes.
  if (pending_notify_reqs_.size() > 1u)
    return;

  // It is possible for some characteristics that support notifications or indications to not have a
  // CCC descriptor. Such characteristics do not need to be directly configured to consider
  // notifications to have been enabled.
  if (ccc_handle_ == att::kInvalidHandle) {
    bt_log(TRACE, "gatt", "notications enabled without characteristic configuration");
    ResolvePendingNotifyRequests(att::Status());
    return;
  }

  StaticByteBuffer<2> ccc_value;
  ccc_value.SetToZeros();

  // Enable indications if supported. Otherwise enable notifications.
  if (info().properties & Property::kIndicate) {
    ccc_value[0] = static_cast<uint8_t>(kCCCIndicationBit);
  } else {
    ccc_value[0] = static_cast<uint8_t>(kCCCNotificationBit);
  }

  auto self = weak_ptr_factory_.GetWeakPtr();
  auto ccc_write_cb = [self](att::Status status) {
    bt_log(DEBUG, "gatt", "CCC write status (enable): %s", status.ToString().c_str());
    if (self) {
      self->ResolvePendingNotifyRequests(status);
    }
  };

  client_->WriteRequest(ccc_handle_, ccc_value, std::move(ccc_write_cb));
}

bool RemoteCharacteristic::DisableNotifications(IdType handler_id) {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  ZX_DEBUG_ASSERT(client_);
  ZX_DEBUG_ASSERT(!shut_down_);

  if (!notify_handlers_.erase(handler_id)) {
    bt_log(TRACE, "gatt", "notify handler not found (id: %lu)", handler_id);
    return false;
  }

  if (!notify_handlers_.empty())
    return true;

  DisableNotificationsInternal();
  return true;
}

void RemoteCharacteristic::DisableNotificationsInternal() {
  if (ccc_handle_ == att::kInvalidHandle) {
    // Nothing to do.
    return;
  }

  if (!client_) {
    bt_log(TRACE, "gatt", "client bearer invalid!");
    return;
  }

  // Disable notifications.
  StaticByteBuffer<2> ccc_value;
  ccc_value.SetToZeros();

  auto ccc_write_cb = [](att::Status status) {
    bt_log(DEBUG, "gatt", "CCC write status (disable): %s", status.ToString().c_str());
  };

  // We send the request without handling the status as there is no good way to
  // recover from failing to disable notifications. If the peer continues to
  // send notifications, they will be dropped as no handlers are registered.
  client_->WriteRequest(ccc_handle_, ccc_value, std::move(ccc_write_cb));
}

void RemoteCharacteristic::ResolvePendingNotifyRequests(att::Status status) {
  // Move the contents of the queue so that a handler can remove itself (this
  // matters when no dispatcher is provided).
  auto pending = std::move(pending_notify_reqs_);
  while (!pending.empty()) {
    auto req = std::move(pending.front());
    pending.pop();

    IdType id = kInvalidId;

    if (status) {
      id = next_notify_handler_id_++;
      notify_handlers_[id] = NotifyHandler(req.dispatcher, std::move(req.value_callback));
    }

    ReportNotifyStatus(status, id, std::move(req.status_callback), req.dispatcher);
  }
}

void RemoteCharacteristic::HandleNotification(const ByteBuffer& value, bool maybe_truncated) {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  ZX_DEBUG_ASSERT(client_);
  ZX_DEBUG_ASSERT(!shut_down_);

  for (auto& iter : notify_handlers_) {
    auto& handler = iter.second;
    NotifyValue(value, maybe_truncated, handler.callback.share(), handler.dispatcher);
  }
}

}  // namespace bt::gatt

// WARNING: This file is machine generated by fidlgen.

#include <fuchsia/hardware/virtioconsole/llcpp/fidl.h>
#include <memory>

namespace llcpp {

namespace fuchsia {
namespace hardware {
namespace virtioconsole {

namespace {

[[maybe_unused]]
constexpr uint64_t kDevice_GetChannel_Ordinal = 0x1ff7011500000000lu;
[[maybe_unused]]
constexpr uint64_t kDevice_GetChannel_GenOrdinal = 0x2add7b2741b2d65dlu;
extern "C" const fidl_type_t fuchsia_hardware_virtioconsole_DeviceGetChannelRequestTable;

}  // namespace

Device::ResultOf::GetChannel_Impl::GetChannel_Impl(zx::unowned_channel _client_end, ::zx::channel req) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<GetChannelRequest, ::fidl::MessageDirection::kSending>();
  ::fidl::internal::AlignedBuffer<_kWriteAllocSize> _write_bytes_inlined;
  auto& _write_bytes_array = _write_bytes_inlined;
  uint8_t* _write_bytes = _write_bytes_array.view().data();
  memset(_write_bytes, 0, GetChannelRequest::PrimarySize);
  auto& _request = *reinterpret_cast<GetChannelRequest*>(_write_bytes);
  _request.req = std::move(req);
  ::fidl::BytePart _request_bytes(_write_bytes, _kWriteAllocSize, sizeof(GetChannelRequest));
  ::fidl::DecodedMessage<GetChannelRequest> _decoded_request(std::move(_request_bytes));
  Super::operator=(
      Device::InPlace::GetChannel(std::move(_client_end), std::move(_decoded_request)));
}

Device::ResultOf::GetChannel Device::SyncClient::GetChannel(::zx::channel req) {
  return ResultOf::GetChannel(zx::unowned_channel(this->channel_), std::move(req));
}

Device::ResultOf::GetChannel Device::Call::GetChannel(zx::unowned_channel _client_end, ::zx::channel req) {
  return ResultOf::GetChannel(std::move(_client_end), std::move(req));
}


Device::UnownedResultOf::GetChannel_Impl::GetChannel_Impl(zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::zx::channel req) {
  if (_request_buffer.capacity() < GetChannelRequest::PrimarySize) {
    Super::status_ = ZX_ERR_BUFFER_TOO_SMALL;
    Super::error_ = ::fidl::internal::kErrorRequestBufferTooSmall;
    return;
  }
  memset(_request_buffer.data(), 0, GetChannelRequest::PrimarySize);
  auto& _request = *reinterpret_cast<GetChannelRequest*>(_request_buffer.data());
  _request.req = std::move(req);
  _request_buffer.set_actual(sizeof(GetChannelRequest));
  ::fidl::DecodedMessage<GetChannelRequest> _decoded_request(std::move(_request_buffer));
  Super::operator=(
      Device::InPlace::GetChannel(std::move(_client_end), std::move(_decoded_request)));
}

Device::UnownedResultOf::GetChannel Device::SyncClient::GetChannel(::fidl::BytePart _request_buffer, ::zx::channel req) {
  return UnownedResultOf::GetChannel(zx::unowned_channel(this->channel_), std::move(_request_buffer), std::move(req));
}

Device::UnownedResultOf::GetChannel Device::Call::GetChannel(zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::zx::channel req) {
  return UnownedResultOf::GetChannel(std::move(_client_end), std::move(_request_buffer), std::move(req));
}

::fidl::internal::StatusAndError Device::InPlace::GetChannel(zx::unowned_channel _client_end, ::fidl::DecodedMessage<GetChannelRequest> params) {
  Device::SetTransactionHeaderFor::GetChannelRequest(params);
  auto _encode_request_result = ::fidl::Encode(std::move(params));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::internal::StatusAndError::FromFailure(
        std::move(_encode_request_result));
  }
  zx_status_t _write_status =
      ::fidl::Write(std::move(_client_end), std::move(_encode_request_result.message));
  if (_write_status != ZX_OK) {
    return ::fidl::internal::StatusAndError(_write_status, ::fidl::internal::kErrorWriteFailed);
  } else {
    return ::fidl::internal::StatusAndError(ZX_OK, nullptr);
  }
}


bool Device::TryDispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
  if (msg->num_bytes < sizeof(fidl_message_header_t)) {
    zx_handle_close_many(msg->handles, msg->num_handles);
    txn->Close(ZX_ERR_INVALID_ARGS);
    return true;
  }
  fidl_message_header_t* hdr = reinterpret_cast<fidl_message_header_t*>(msg->bytes);
  switch (hdr->ordinal) {
    case kDevice_GetChannel_Ordinal:
    case kDevice_GetChannel_GenOrdinal:
    {
      auto result = ::fidl::DecodeAs<GetChannelRequest>(msg);
      if (result.status != ZX_OK) {
        txn->Close(ZX_ERR_INVALID_ARGS);
        return true;
      }
      auto message = result.message.message();
      impl->GetChannel(std::move(message->req),
        Interface::GetChannelCompleter::Sync(txn));
      return true;
    }
    default: {
      return false;
    }
  }
}

bool Device::Dispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
  bool found = TryDispatch(impl, msg, txn);
  if (!found) {
    zx_handle_close_many(msg->handles, msg->num_handles);
    txn->Close(ZX_ERR_NOT_SUPPORTED);
  }
  return found;
}



void Device::SetTransactionHeaderFor::GetChannelRequest(const ::fidl::DecodedMessage<Device::GetChannelRequest>& _msg) {
  ::fidl::InitializeTransactionHeader(&_msg.message()->_hdr);
  _msg.message()->_hdr.ordinal = kDevice_GetChannel_GenOrdinal;
}

}  // namespace virtioconsole
}  // namespace hardware
}  // namespace fuchsia
}  // namespace llcpp

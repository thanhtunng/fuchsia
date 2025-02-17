// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_CPP_MESSAGE_H_
#define LIB_FIDL_CPP_MESSAGE_H_

#include <lib/fidl/coding.h>
#include <lib/fidl/cpp/message_part.h>
#include <lib/fidl/internal.h>
#include <lib/fidl/txn_header.h>
#include <zircon/fidl.h>

#include <vector>

namespace fidl {

// An incoming FIDL message.
//
// A FIDL message has two parts: the bytes and the handles. The bytes are
// divided into a header (of type fidl_message_header_t) and a payload, which
// follows the header.
//
// A HLCPPIncomingMessage object does not own the storage for the message parts.
class HLCPPIncomingMessage {
 public:
  // Creates a message without any storage.
  HLCPPIncomingMessage();

  // Creates a message whose storage is backed by |bytes| and |handles|.
  //
  // The constructed |Message| object does not take ownership of the given
  // storage, although does take ownership of zircon handles contained withing
  // handles.
  HLCPPIncomingMessage(BytePart bytes, HandleInfoPart handles);

  ~HLCPPIncomingMessage();

  HLCPPIncomingMessage(const HLCPPIncomingMessage& other) = delete;
  HLCPPIncomingMessage& operator=(const HLCPPIncomingMessage& other) = delete;

  HLCPPIncomingMessage(HLCPPIncomingMessage&& other);
  HLCPPIncomingMessage& operator=(HLCPPIncomingMessage&& other);

  // The header at the start of the message.
  const fidl_message_header_t& header() const {
    return *reinterpret_cast<fidl_message_header_t*>(bytes_.data());
  }
  fidl_message_header_t& header() {
    return *reinterpret_cast<fidl_message_header_t*>(bytes_.data());
  }

  // The transaction ID in the message header.
  zx_txid_t txid() const { return header().txid; }
  void set_txid(zx_txid_t txid) { header().txid = txid; }

  // The ordinal in the message header.
  uint64_t ordinal() const { return header().ordinal; }

  // Whether this message is in a supported version of the wire format.
  bool is_supported_version() const {
    return fidl_validate_txn_header(GetBytesAs<fidl_message_header_t>()) == ZX_OK;
  }

  // The message payload that follows the header.
  BytePart payload() const {
    constexpr uint32_t n = sizeof(fidl_message_header_t);
    return BytePart(bytes_.data() + n, bytes_.capacity() - n, bytes_.actual() - n);
  }

  // The message bytes interpreted as the given type.
  template <typename T>
  T* GetBytesAs() const {
    return reinterpret_cast<T*>(bytes_.data());
  }

  // The message payload that follows the header interpreted as the given type.
  template <typename T>
  T* GetPayloadAs() const {
    return reinterpret_cast<T*>(bytes_.data() + sizeof(fidl_message_header_t));
  }

  // The storage for the bytes of the message.
  BytePart& bytes() { return bytes_; }
  const BytePart& bytes() const { return bytes_; }
  void set_bytes(BytePart bytes) { bytes_ = static_cast<BytePart&&>(bytes); }

  // The storage for the handles of the message.
  //
  // When the message is encoded, the handle values are stored in this part of
  // the message. When the message is decoded, this part of the message is
  // empty and the handle values are stored in the bytes().
  HandleInfoPart& handles() { return handles_; }
  const HandleInfoPart& handles() const { return handles_; }

  // Decodes the message in-place.
  //
  // The message must previously have been in an encoded state, for example,
  // either by being read from a zx_channel_t or having been encoded using the
  // |Encode| method.
  zx_status_t Decode(const fidl_type_t* type, const char** error_msg_out);

  // Decodes the message in-place when the header is not part of the byte
  // message and instead specified as an argument. Otherwise identical to
  // Decode().
  //
  // This function MAY BREAK OR BE REMOVED AT ANY TIME. DO NOT USE IT.
  zx_status_t DecodeWithExternalHeader_InternalMayBreak(const fidl_message_header_t& header,
                                                        const fidl_type_t* type,
                                                        const char** error_msg_out);

  // Read a message from the given channel.
  //
  // The bytes read from the channel are stored in bytes() and the handles
  // read from the channel are stored in handles(). Existing data in these
  // buffers is overwritten.
  zx_status_t Read(zx_handle_t channel, uint32_t flags);

  // Stop tracking the handles in stored in handles(), without closing them.
  //
  // Typically, these handles will be extracted during decode or the
  // message's destructor, so this function will be unnecessary. However,
  // for clients of ulib/fidl which decode message manually, this function
  // is necessary to prevent extracted handles from being closed.
  void ClearHandlesUnsafe();

 private:
  BytePart bytes_;
  HandleInfoPart handles_;
};

// An outgoing FIDL message.
//
// A FIDL message has two parts: the bytes and the handles. The bytes are
// divided into a header (of type fidl_message_header_t) and a payload, which
// follows the header.
//
// A HLCPPOutgoingMessage object does not own the storage for the message parts.
class HLCPPOutgoingMessage {
 public:
  // Creates a message without any storage.
  HLCPPOutgoingMessage();

  // Creates a message whose storage is backed by |bytes| and |handles|.
  //
  // The constructed |Message| object does not take ownership of the given
  // storage, although does take ownership of zircon handles contained withing
  // handles.
  HLCPPOutgoingMessage(BytePart bytes, HandleDispositionPart handles);

  ~HLCPPOutgoingMessage();

  HLCPPOutgoingMessage(const HLCPPOutgoingMessage& other) = delete;
  HLCPPOutgoingMessage& operator=(const HLCPPOutgoingMessage& other) = delete;

  HLCPPOutgoingMessage(HLCPPOutgoingMessage&& other);
  HLCPPOutgoingMessage& operator=(HLCPPOutgoingMessage&& other);

  // The header at the start of the message.
  const fidl_message_header_t& header() const {
    return *reinterpret_cast<fidl_message_header_t*>(bytes_.data());
  }
  fidl_message_header_t& header() {
    return *reinterpret_cast<fidl_message_header_t*>(bytes_.data());
  }

  // The transaction ID in the message header.
  zx_txid_t txid() const { return header().txid; }
  void set_txid(zx_txid_t txid) { header().txid = txid; }

  // The ordinal in the message header.
  uint64_t ordinal() const { return header().ordinal; }

  // The message payload that follows the header.
  BytePart payload() const {
    constexpr uint32_t n = sizeof(fidl_message_header_t);
    return BytePart(bytes_.data() + n, bytes_.capacity() - n, bytes_.actual() - n);
  }

  // The storage for the bytes of the message.
  BytePart& bytes() { return bytes_; }
  const BytePart& bytes() const { return bytes_; }
  void set_bytes(BytePart bytes) { bytes_ = static_cast<BytePart&&>(bytes); }

  // The storage for the handles of the message.
  //
  // When the message is encoded, the handle values are stored in this part of
  // the message. When the message is decoded, this part of the message is
  // empty and the handle values are stored in the bytes().
  HandleDispositionPart& handles() { return handles_; }
  const HandleDispositionPart& handles() const { return handles_; }

  // Encodes the message in-place.
  //
  // The message must previously have been in a decoded state, for example,
  // either by being built in a decoded state using a |Builder| or having been
  // decoded using the |Decode| method.
  zx_status_t Encode(const fidl_type_t* type, const char** error_msg_out);

  // Validates the message in-place.
  //
  // Assumes a message header is present.
  //
  // The message must already be in an encoded state, for example, either by
  // being read from a zx_channel_t or having been created in that state.
  //
  // Does not modify the message.
  zx_status_t Validate(const fidl_type_t* type, const char** error_msg_out) const;

  // Validates the message in-place.
  //
  // The wire format version is provided as an argument for internal tests.
  //
  // The message must already be in an encoded state, for example, either by
  // being read from a zx_channel_t or having been created in that state.
  //
  // Does not modify the message.
  zx_status_t ValidateWithVersion_InternalMayBreak(internal::WireFormatVersion wire_format_version,
                                                   const fidl_type_t* v1_type,
                                                   const char** error_msg_out) const;

  // Writes a message to the given channel.
  //
  // The bytes stored in bytes() are written to the channel and the handles
  // stored in handles() are written to the channel.
  //
  // If this method returns ZX_OK, handles() will be empty because they were
  // consumed by this operation.
  zx_status_t Write(zx_handle_t channel, uint32_t flags);

  // Issues a synchronous send and receive transaction on the given channel.
  //
  // The bytes stored in bytes() are written to the channel and the handles
  // stored in handles() are written to the channel. The bytes read from the
  // channel are stored in response->bytes() and the handles read from the
  // channel are stored in response->handles().
  //
  // If this method returns ZX_OK, handles() will be empty because they were
  // consumed by this operation.
  zx_status_t Call(zx_handle_t channel, uint32_t flags, zx_time_t deadline,
                   HLCPPIncomingMessage* response);

  // Stop tracking the handles in stored in handles(), without closing them.
  //
  // Typically, these handles will be extracted during decode or the
  // message's destructor, so this function will be unnecessary. However,
  // for clients of ulib/fidl which decode message manually, this function
  // is necessary to prevent extracted handles from being closed.
  void ClearHandlesUnsafe();

 private:
  BytePart bytes_;
  HandleDispositionPart handles_;
};

}  // namespace fidl

#endif  // LIB_FIDL_CPP_MESSAGE_H_

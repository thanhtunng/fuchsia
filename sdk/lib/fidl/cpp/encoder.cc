// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/fidl/cpp/encoder.h"

#include <lib/fidl/txn_header.h>
#include <zircon/assert.h>
#include <zircon/fidl.h>

namespace fidl {
namespace internal {
std::atomic_int hlcpp_enable_v2_encode(0);
}

namespace {

size_t Align(size_t size) {
  constexpr size_t alignment_mask = FIDL_ALIGNMENT - 1;
  return (size + alignment_mask) & ~alignment_mask;
}

}  // namespace

Encoder::Encoder(uint64_t ordinal) { EncodeMessageHeader(ordinal); }

Encoder::~Encoder() = default;

const size_t kSmallAllocSize = 512;
const size_t kLargeAllocSize = 65536;

size_t Encoder::Alloc(size_t size) {
  size_t offset = bytes_.size();
  size_t new_size = bytes_.size() + Align(size);

  if (likely(new_size <= kSmallAllocSize)) {
    bytes_.reserve(kSmallAllocSize);
  } else if (likely(new_size <= kLargeAllocSize)) {
    bytes_.reserve(kLargeAllocSize);
  } else {
    bytes_.reserve(new_size);
  }
  bytes_.resize(new_size);

  return offset;
}

#ifdef __Fuchsia__
void Encoder::EncodeHandle(zx::object_base* value, zx_obj_type_t obj_type, zx_rights_t rights,
                           size_t offset) {
  if (value->is_valid()) {
    *GetPtr<zx_handle_t>(offset) = FIDL_HANDLE_PRESENT;
    handles_.push_back(zx_handle_disposition_t{
        .operation = ZX_HANDLE_OP_MOVE,
        .handle = value->release(),
        .type = obj_type,
        .rights = rights,
        .result = ZX_OK,
    });
  } else {
    *GetPtr<zx_handle_t>(offset) = FIDL_HANDLE_ABSENT;
  }
}

void Encoder::EncodeUnknownHandle(zx::object_base* value) {
  if (value->is_valid()) {
    handles_.push_back(zx_handle_disposition_t{
        .operation = ZX_HANDLE_OP_MOVE,
        .handle = value->release(),
        .type = ZX_OBJ_TYPE_NONE,
        .rights = ZX_RIGHT_SAME_RIGHTS,
        .result = ZX_OK,
    });
  }
}
#endif

HLCPPOutgoingMessage Encoder::GetMessage() {
  return HLCPPOutgoingMessage(
      BytePart(bytes_.data(), static_cast<uint32_t>(bytes_.size()),
               static_cast<uint32_t>(bytes_.size())),
      HandleDispositionPart(handles_.data(), static_cast<uint32_t>(handles_.size()),
                            static_cast<uint32_t>(handles_.size())));
}

void Encoder::Reset(uint64_t ordinal) {
  bytes_.clear();
  handles_.clear();
  EncodeMessageHeader(ordinal);
}

void Encoder::EncodeMessageHeader(uint64_t ordinal) {
  size_t offset = Alloc(sizeof(fidl_message_header_t));
  fidl_message_header_t* header = GetPtr<fidl_message_header_t>(offset);
  fidl_init_txn_header(header, 0, ordinal);
  if (wire_format() == internal::WireFormatVersion::kV2) {
    header->flags[0] |= FIDL_MESSAGE_HEADER_FLAGS_0_USE_VERSION_V2;
  }
}

}  // namespace fidl

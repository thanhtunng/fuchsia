// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_STORAGE_PUBLIC_CONSTANTS_H_
#define APPS_LEDGER_SRC_STORAGE_PUBLIC_CONSTANTS_H_

#include <stdint.h>

#include "lib/fxl/strings/string_view.h"

namespace storage {

// The size of a commit id in number of bytes.
constexpr uint64_t kCommitIdSize = 32;

// The id of the first commit of a page.
constexpr char kFirstPageCommitIdArray[kCommitIdSize] = {0};
constexpr const fxl::StringView kFirstPageCommitId(kFirstPageCommitIdArray,
                                                   kCommitIdSize);

// The serialization version of the ledger.
constexpr const fxl::StringView kSerializationVersion = "17";

}  // namespace storage

#endif  // APPS_LEDGER_SRC_STORAGE_PUBLIC_CONSTANTS_H_

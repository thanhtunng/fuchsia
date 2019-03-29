// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

//
//
//

#include <stdbool.h>
#include <stdint.h>

//
//
//

bool     is_pow2_u32(uint32_t n);
uint32_t pow2_ru_u32(uint32_t n);
uint32_t pow2_rd_u32(uint32_t n);
uint32_t msb_idx_u32(uint32_t n); // 0-based bit position

//
//
//

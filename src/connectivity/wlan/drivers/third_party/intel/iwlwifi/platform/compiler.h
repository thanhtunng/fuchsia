// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_PLATFORM_COMPILER_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_PLATFORM_COMPILER_H_

// This file contains Fuchsia-specific compiler support code.

#include <endian.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <zircon/compiler.h>

typedef uint32_t __be32;
typedef uint16_t __be16;
typedef uint64_t __le64;
typedef uint32_t __le32;
typedef uint16_t __le16;
typedef uint8_t __s8;
typedef uint8_t __u8;

#if defined(__cplusplus)
extern "C++" {
#include <atomic>
}  // extern "C++"
#define _Atomic(T) std::atomic<T>
using std::memory_order_relaxed;
using std::memory_order_seq_cst;
#else  // defined(__cplusplus)
#include <stdatomic.h>
#endif

typedef struct {
  _Atomic(int32_t) value;
} atomic_t;

typedef struct {
  _Atomic(int64_t) value;
} atomic64_t;

#if defined(__cplusplus)
extern "C" {
#endif  // defined(__cplusplus)

#define BIT(x) (1UL << (x))

#define DIV_ROUND_UP(num, div) (((num) + (div)-1) / (div))

// Endianness byteswap macros.
#define le16_to_cpu(x) le16toh(x)
#define le32_to_cpu(x) le32toh(x)
#define le64_to_cpu(x) le64toh(x)
#define cpu_to_le16(x) htole16(x)
#define cpu_to_le32(x) htole32(x)
#define cpu_to_le64(x) htole64(x)

// Endianness access of possibly unaligned data.
static inline uint16_t le16_to_cpup(const uint16_t* x) {
  uint16_t val = 0;
  memcpy(&val, x, sizeof(val));
  return le16toh(val);
}

static inline uint32_t le32_to_cpup(const uint32_t* x) {
  uint32_t val = 0;
  memcpy(&val, x, sizeof(val));
  return le32toh(val);
}

static inline uint16_t be16_to_cpup(const uint16_t* x) {
  uint16_t val = 0;
  memcpy(&val, x, sizeof(val));
  return be16toh(val);
}

#define lower_32_bits(x) (x & 0xffffffff)
#define upper_32_bits(x) (x >> 32)

#define BITS_PER_BYTE 8
#define BITS_PER_INT (sizeof(int) * BITS_PER_BYTE)
#define BITS_PER_LONG (sizeof(long) * BITS_PER_BYTE)

#define BITS_TO_INTS(nr) (DIV_ROUND_UP(nr, BITS_PER_INT))
#define BITS_TO_LONGS(nr) (DIV_ROUND_UP(nr, BITS_PER_LONG))

#define __aligned(x) __attribute__((aligned(x)))
#define __force
#define __must_check __attribute__((warn_unused_result))
#define __packed __PACKED
#define __rcu                         // NEEDS_TYPES
#define ____cacheline_aligned_in_smp  // NEEDS_TYPES

// NEEDS_TYPES: Need to check if 'x' is static array.
#define ARRAY_SIZE(x) (countof(x))

#define offsetofend(type, member) (offsetof(type, member) + sizeof(((type*)NULL)->member))

// NEEDS_TYPES: need to be generic
// clang-format off
#define roundup_pow_of_two(x)  \
  (x >= 0x100 ? 0xFFFFFFFF :   \
   x >= 0x080 ? 0x100 :        \
   x >= 0x040 ? 0x080 :        \
   x >= 0x020 ? 0x040 :        \
   x >= 0x010 ? 0x020 :        \
   x >= 0x008 ? 0x010 :        \
   x >= 0x004 ? 0x008 :        \
   x >= 0x002 ? 0x004 :        \
   x >= 0x001 ? 0x002 : 1)
// clang-format on
#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define ROUND_UP(x, y) ((((x)-1) | __round_mask(x, y)) + 1)
#define ROUND_DOWN(x, y) ((x) & ~__round_mask(x, y))

static inline void __set_bit(unsigned long bit, volatile unsigned long* addr) {
  volatile unsigned long* p = addr + (bit / BITS_PER_LONG);
  const unsigned long mask = 1ul << (bit % BITS_PER_LONG);
  *p |= mask;
}

// Atomic operations.  DANGER DANGER DANGER HERE BE DRAGONS
// These implementations are heavily informed by ISO/IEC JTC1 SC22 WG21 P0124R7
//   http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p0124r7.html

static inline void set_bit(unsigned long bit, volatile unsigned long* addr) {
  volatile unsigned long* const p = addr + (bit / BITS_PER_LONG);
  const unsigned long mask = 1ul << (bit % BITS_PER_LONG);
  atomic_fetch_or_explicit((_Atomic(unsigned long)*)p, mask, memory_order_relaxed);
}

static inline void clear_bit(unsigned long bit, volatile unsigned long* addr) {
  volatile unsigned long* const p = addr + (bit / BITS_PER_LONG);
  const unsigned long mask = 1ul << (bit % BITS_PER_LONG);
  atomic_fetch_and_explicit((_Atomic(unsigned long)*)p, ~mask, memory_order_relaxed);
}

static inline int test_bit(unsigned long bit, const volatile unsigned long* addr) {
  const volatile unsigned long* const p = addr + (bit / BITS_PER_LONG);
  const unsigned long mask = 1ul << (bit % BITS_PER_LONG);
  atomic_thread_fence(memory_order_seq_cst);
  const unsigned long value =
      atomic_load_explicit((_Atomic(unsigned long)*)p, memory_order_relaxed);
  atomic_thread_fence(memory_order_seq_cst);
  return !!(value & mask);
}

static inline int test_and_set_bit(unsigned long bit, volatile unsigned long* addr) {
  volatile unsigned long* const p = addr + (bit / BITS_PER_LONG);
  const unsigned long mask = 1ul << (bit % BITS_PER_LONG);
  atomic_thread_fence(memory_order_seq_cst);
  const unsigned long value =
      atomic_fetch_or_explicit((_Atomic(unsigned long)*)p, mask, memory_order_relaxed);
  atomic_thread_fence(memory_order_seq_cst);
  return !!(value & mask);
}

static inline int test_and_clear_bit(unsigned long bit, volatile unsigned long* addr) {
  volatile unsigned long* const p = addr + (bit / BITS_PER_LONG);
  const unsigned long mask = 1ul << (bit % BITS_PER_LONG);
  atomic_thread_fence(memory_order_seq_cst);
  const unsigned long value =
      atomic_fetch_and_explicit((_Atomic(unsigned long)*)p, ~mask, memory_order_relaxed);
  atomic_thread_fence(memory_order_seq_cst);
  return !!(value & mask);
}

static inline int32_t atomic_read(const atomic_t* atomic) {
  return atomic_load_explicit(&atomic->value, memory_order_relaxed);
}

static inline void atomic_set(atomic_t* atomic, int32_t value) {
  atomic_store_explicit(&atomic->value, value, memory_order_relaxed);
}

static inline void atomic_inc(atomic_t* atomic) {
  atomic_fetch_add_explicit(&atomic->value, 1, memory_order_relaxed);
}

static inline int32_t atomic_xchg(atomic_t* atomic, int32_t value) {
  atomic_thread_fence(memory_order_seq_cst);
  const int32_t old = atomic_exchange_explicit(&atomic->value, value, memory_order_seq_cst);
  atomic_thread_fence(memory_order_seq_cst);
  return old;
}

static inline int32_t atomic_dec_if_positive(atomic_t* atomic) {
  int32_t current = atomic_load_explicit(&atomic->value, memory_order_relaxed);
  while (1) {
    if (current <= 0) {
      return current;
    }
    if (atomic_compare_exchange_weak_explicit(&atomic->value, &current, current - 1,
                                              memory_order_seq_cst, memory_order_relaxed)) {
      atomic_thread_fence(memory_order_seq_cst);
      return current - 1;
    }
  }
}

static inline int64_t atomic64_inc_return(atomic64_t* atomic) {
  atomic_thread_fence(memory_order_seq_cst);
  const int64_t old = atomic_fetch_add_explicit(&atomic->value, 1, memory_order_seq_cst);
  atomic_thread_fence(memory_order_seq_cst);
  return old + 1;
}

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define max_t(type, a, b) MAX((type)(a), (type)(b))
#define min_t(type, a, b) MIN((type)(a), (type)(b))

// Find the first asserted LSB.
//
// Returns:
//   [0, num_bits): found. The index of first asserted bit (the least significant one.
//   num_bits: No asserted bit found in num_bits.
//
static inline size_t find_first_bit(unsigned* bits, const size_t num_bits) {
  const size_t num_of_ints = DIV_ROUND_UP(num_bits, BITS_PER_INT);
  size_t ret = num_bits;

  for (size_t i = 0; i < num_of_ints; ++i) {
    if (bits[i] == 0) {
      continue;
    }
    ret = (i * BITS_PER_INT) + __builtin_ctz(bits[i]);
    break;
  }

  return MIN(num_bits, ret);
}

#if defined(__cplusplus)
}  // extern "C"
#endif  // defined(__cplusplus)

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_PLATFORM_COMPILER_H_

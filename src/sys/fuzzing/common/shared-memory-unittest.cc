// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/fuzzing/common/shared-memory.h"

#include <zircon/errors.h>

#include <limits>
#include <random>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fuzzing {
namespace {

using ::testing::Eq;
using ::testing::Ne;

// Test fixtures.

constexpr size_t kCapacity = 0x1000;

// Helper function to create a deterministically pseudorandom integer type.
template <typename T>
T Pick() {
  static std::mt19937_64 prng;
  return static_cast<T>(prng() & std::numeric_limits<T>::max());
}

// Helper function to create an array of deterministically pseudorandom integer types.
template <typename T>
void PickArray(T* out, size_t out_len) {
  for (size_t i = 0; i < out_len; ++i) {
    out[i] = Pick<T>();
  }
}

// Helper function to create a vector of deterministically pseudorandom integer types.
template <typename T = uint8_t>
std::vector<T> PickVector(size_t size) {
  std::vector<T> v;
  v.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    v.push_back(Pick<T>());
  }
  return v;
}

// Unit tests.

TEST(SharedMemoryTest, Create) {
  Buffer buffer;
  SharedMemory shmem;

  shmem.Create(kCapacity, &buffer);
  EXPECT_TRUE(shmem.is_mapped());
  EXPECT_EQ(shmem.capacity(), kCapacity);
  EXPECT_TRUE(buffer.vmo.is_valid());
  EXPECT_EQ(buffer.size, kCapacity);

  // Can recreate
  shmem.Create(kCapacity * 2, &buffer);
  EXPECT_TRUE(shmem.is_mapped());
  EXPECT_EQ(shmem.capacity(), kCapacity * 2);
  EXPECT_TRUE(buffer.vmo.is_valid());
  EXPECT_EQ(buffer.size, kCapacity * 2);
}

TEST(SharedMemoryTest, Share) {
  Buffer buffer;
  SharedMemory shmem;
  uint8_t region[kCapacity * 2];

  shmem.Share(&region[0], &region[kCapacity], &buffer);
  EXPECT_TRUE(shmem.is_mapped());
  EXPECT_EQ(shmem.capacity(), kCapacity);
  EXPECT_TRUE(buffer.vmo.is_valid());
  EXPECT_EQ(buffer.size, kCapacity);

  // Can recreate
  shmem.Share(region, region + sizeof(region) / sizeof(region[0]), &buffer);
  EXPECT_TRUE(shmem.is_mapped());
  EXPECT_EQ(shmem.capacity(), kCapacity * 2);
  EXPECT_TRUE(buffer.vmo.is_valid());
  EXPECT_EQ(buffer.size, kCapacity * 2);
}

TEST(SharedMemoryTest, Link) {
  SharedMemory shmem;

  Buffer buffer1;
  buffer1.size = kCapacity;
  EXPECT_EQ(zx::vmo::create(buffer1.size, 0, &buffer1.vmo), ZX_OK);
  shmem.Link(std::move(buffer1));
  EXPECT_TRUE(shmem.is_mapped());
  EXPECT_EQ(shmem.capacity(), kCapacity);

  // Can remap.
  Buffer buffer2;
  buffer2.size = kCapacity * 2;
  EXPECT_EQ(zx::vmo::create(buffer2.size, 0, &buffer2.vmo), ZX_OK);
  shmem.Link(std::move(buffer2));
  EXPECT_TRUE(shmem.is_mapped());
  EXPECT_EQ(shmem.capacity(), kCapacity * 2);
}

TEST(SharedMemoryTest, Reset) {
  Buffer buffer;
  SharedMemory shmem;

  // Valid even if unmapped
  shmem.Reset();

  // Valid
  shmem.Create(kCapacity, &buffer);
  EXPECT_TRUE(shmem.is_mapped());
  shmem.Reset();
  EXPECT_FALSE(shmem.is_mapped());

  // Can map again after reset.
  shmem.Create(kCapacity, &buffer);
  EXPECT_TRUE(shmem.is_mapped());
}

TEST(SharedMemoryTest, Write) {
  Buffer buffer;
  SharedMemory shmem;

  // No VMO is mapped.
  EXPECT_EQ(shmem.size(), 0u);
  auto expected = PickVector((kCapacity / 2) + 1);
  EXPECT_EQ(shmem.Write(expected.data(), expected.size()), ZX_ERR_BAD_STATE);
  EXPECT_EQ(shmem.size(), 0u);

  // Valid
  shmem.Create(kCapacity, &buffer, /* inline_size= */ true);
  EXPECT_EQ(shmem.size(), 0u);
  EXPECT_EQ(shmem.Write(expected.data(), expected.size()), ZX_OK);
  EXPECT_EQ(shmem.size(), expected.size());

  SharedMemory other;
  other.Link(std::move(buffer), /* inline_size= */ true);
  EXPECT_EQ(other.size(), expected.size());
  std::vector<uint8_t> actual(other.data(), other.data() + other.size());
  EXPECT_THAT(actual, Eq(expected));

  // Capped at capacity.
  while (expected.size() <= shmem.capacity() - shmem.size()) {
    EXPECT_EQ(shmem.Write(expected.data(), expected.size()), ZX_OK);
  }
  EXPECT_EQ(shmem.Write(expected.data(), expected.size()), ZX_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(shmem.size(), shmem.capacity());
  EXPECT_EQ(shmem.Write(expected.data(), 1), ZX_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(shmem.size(), shmem.capacity());
}

TEST(SharedMemoryTest, Update) {
  Buffer buffer;
  SharedMemory shmem;

  // No-op when not shared.
  shmem.Create(kCapacity, &buffer);
  const auto& vmo = shmem.vmo();
  auto addr = shmem.addr();
  auto capacity = shmem.capacity();
  auto is_mapped = shmem.is_mapped();
  auto* begin = shmem.begin();
  auto* end = shmem.end();
  auto* data = shmem.data();
  auto size = shmem.size();
  shmem.Update();
  EXPECT_EQ(vmo, shmem.vmo());
  EXPECT_EQ(addr, shmem.addr());
  EXPECT_EQ(capacity, shmem.capacity());
  EXPECT_EQ(is_mapped, shmem.is_mapped());
  EXPECT_EQ(begin, shmem.begin());
  EXPECT_EQ(end, shmem.end());
  EXPECT_EQ(data, shmem.data());
  EXPECT_EQ(size, shmem.size());

  // Valid
  auto expected = PickVector(kCapacity);
  shmem.Share(expected.data(), expected.data() + expected.size(), &buffer);
  EXPECT_EQ(shmem.size(), kCapacity);

  SharedMemory other;
  other.Link(std::move(buffer));
  EXPECT_EQ(other.size(), expected.size());
  std::vector<uint8_t> actual(other.data(), other.data() + other.size());
  EXPECT_THAT(actual, Eq(expected));

  //  Change source data, but don't update. Uses |cksum| to verify |expected| did in fact change.
  auto cksum = std::accumulate(expected.begin(), expected.end(), 0, std::bit_xor<>());
  PickArray(expected.data(), expected.size());
  EXPECT_NE(cksum, std::accumulate(expected.begin(), expected.end(), 0, std::bit_xor<>()));
  actual = std::vector<uint8_t>(other.data(), other.data() + other.size());
  EXPECT_THAT(actual, Ne(expected));

  //  Now update
  shmem.Update();
  actual = std::vector<uint8_t>(other.data(), other.data() + other.size());
  EXPECT_THAT(actual, Eq(expected));
}

TEST(SharedMemoryTest, Clear) {
  Buffer buffer;
  SharedMemory shmem;
  EXPECT_EQ(shmem.size(), 0u);

  // VMO does not have to be mapped.
  shmem.Clear();
  EXPECT_EQ(shmem.size(), 0u);

  auto expected = PickVector(kCapacity);
  shmem.Create(kCapacity, &buffer, /* inline_size= */ true);
  EXPECT_EQ(shmem.Write(expected.data(), expected.size()), ZX_OK);

  SharedMemory other;
  other.Link(std::move(buffer), /* inline_size= */ true);
  std::vector<uint8_t> actual(other.data(), other.data() + other.size());
  EXPECT_THAT(actual, Eq(expected));

  // Valid
  shmem.Clear();
  EXPECT_EQ(shmem.size(), 0u);

  // Can write after clearing.
  expected = PickVector(kCapacity);
  EXPECT_EQ(shmem.Write(expected.data(), expected.size()), ZX_OK);
  actual = std::vector<uint8_t>(other.data(), other.data() + other.size());
  EXPECT_THAT(actual, Eq(expected));
}

}  // namespace
}  // namespace fuzzing

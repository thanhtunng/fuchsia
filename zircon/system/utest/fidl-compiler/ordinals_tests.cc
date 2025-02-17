// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "error_test.h"
#include "test_library.h"

#define BORINGSSL_NO_CXX
#include <cinttypes>
#include <regex>

#include <openssl/sha.h>
#include <zxtest/zxtest.h>

namespace {

// Since a number of these tests rely on specific properties of 64b hashes
// which are computationally prohibitive to reverse engineer, we rely on
// a stubbed out method hasher `GetGeneratedOrdinal64ForTesting` defined
// in test_library.h.

TEST(OrdinalsTests, BadOrdinalCannotBeZero) {
  TestLibrary library(R"FIDL(
library methodhasher;

protocol Special {
    ThisOneHashesToZero() -> (struct { i int64; });
};

)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrGeneratedZeroValueOrdinal);
}

TEST(OrdinalsTests, BadClashingOrdinalValues) {
  auto library = WithLibraryZx(R"FIDL(
library methodhasher;

using zx;

protocol Special {
    ClashOne(struct { s string; b bool; }) -> (struct { i int32; });
    ClashTwo(struct { s string; }) -> (struct { r zx.handle:CHANNEL; });
};

)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrDuplicateMethodOrdinal);

  // The FTP requires the error message as follows
  const std::regex pattern(R"REGEX(\[\s*Selector\s*=\s*"(ClashOne|ClashTwo)_"\s*\])REGEX");
  std::smatch sm;
  std::string error_msg(library.errors()[0]->msg);
  ASSERT_TRUE(std::regex_search(error_msg, sm, pattern), "%s",
              ("Selector pattern not found in error: " + library.errors()[0]->msg).c_str());
}

TEST(OrdinalsTests, BadClashingOrdinalValuesWithAttribute) {
  auto library = WithLibraryZx(R"FIDL(
library methodhasher;

using zx;

protocol Special {
    @selector("ClashOne")
    foo(struct { s string; b bool; }) -> (struct { i int32; });
    @selector("ClashTwo")
    bar(struct { s string; }) -> (struct { r zx.handle:CHANNEL; });
};

)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrDuplicateMethodOrdinal);

  // The FTP requires the error message as follows
  const std::regex pattern(R"REGEX(\[\s*Selector\s*=\s*"(ClashOne|ClashTwo)_"\s*\])REGEX");
  std::smatch sm;
  std::string error_msg(library.errors()[0]->msg);
  ASSERT_TRUE(std::regex_search(error_msg, sm, pattern), "%s",
              ("Selector pattern not found in error: " + library.errors()[0]->msg).c_str());
}

TEST(OrdinalsTests, GoodAttributeResolvesClashes) {
  auto library = WithLibraryZx(R"FIDL(
library methodhasher;

using zx;

protocol Special {
    @selector("ClashOneReplacement")
    ClashOne(struct { s string; b bool; }) -> (struct { i int32; });
    ClashTwo(struct { s string; }) -> (resource struct { r zx.handle:CHANNEL; });
};

)FIDL");
  ASSERT_COMPILED(library);
}

TEST(OrdinalsTests, GoodOrdinalValueIsSha256) {
  TestLibrary library(R"FIDL(library a.b.c;

protocol protocol {
    selector(struct {
        s string;
        b bool;
    }) -> (struct {
        i int32;
    });
};
)FIDL");
  ASSERT_COMPILED(library);

  const char hash_name64[] = "a.b.c/protocol.selector";
  uint8_t digest64[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(hash_name64), strlen(hash_name64), digest64);
  uint64_t expected_hash64 = *(reinterpret_cast<uint64_t*>(digest64)) & 0x7fffffffffffffff;

  const fidl::flat::Protocol* iface = library.LookupProtocol("protocol");
  uint64_t actual_hash64 = iface->methods[0].generated_ordinal64->value;
  ASSERT_EQ(actual_hash64, expected_hash64, "Expected 64bits hash is not correct");
}

TEST(OrdinalsTests, GoodSelectorWithFullPath) {
  TestLibrary library(R"FIDL(library not.important;

protocol at {
    @selector("a.b.c/protocol.selector")
    all();
};
)FIDL");
  ASSERT_COMPILED(library);

  const char hash_name64[] = "a.b.c/protocol.selector";
  uint8_t digest64[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(hash_name64), strlen(hash_name64), digest64);
  uint64_t expected_hash64 = *(reinterpret_cast<uint64_t*>(digest64)) & 0x7fffffffffffffff;

  const fidl::flat::Protocol* iface = library.LookupProtocol("at");
  uint64_t actual_hash64 = iface->methods[0].generated_ordinal64->value;
  ASSERT_EQ(actual_hash64, expected_hash64, "Expected 64bits hash is not correct");
}

TEST(OrdinalsTests, BadSelectorValueIsValidated) {
  TestLibrary library(R"FIDL(
library not.important;

protocol at {
    // missing two components after the slash
    @selector("a.b.c/selector")
    all();
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidSelectorValue);
}

// generated by gen_ordinal_value_is_first64bits_of_sha256_test.sh
TEST(OrdinalsTests, GoodOrdinalValueIsFirst64BitsOfSha256) {
  TestLibrary library(R"FIDL(library a.b.c;

protocol protocol {
    s0();
    s1();
    s2();
    s3();
    s4();
    s5();
    s6();
    s7();
    s8();
    s9();
    s10();
    s11();
    s12();
    s13();
    s14();
    s15();
    s16();
    s17();
    s18();
    s19();
    s20();
    s21();
    s22();
    s23();
    s24();
    s25();
    s26();
    s27();
    s28();
    s29();
    s30();
    s31();
};
)FIDL");
  ASSERT_COMPILED(library);

  const fidl::flat::Protocol* iface = library.LookupProtocol("protocol");
  EXPECT_EQ(iface->methods[0].generated_ordinal64->value, 0x3b1625372e15f1ae);
  EXPECT_EQ(iface->methods[1].generated_ordinal64->value, 0x4199e504fa71b5a4);
  EXPECT_EQ(iface->methods[2].generated_ordinal64->value, 0x247ca8a890628135);
  EXPECT_EQ(iface->methods[3].generated_ordinal64->value, 0x64f7c02cfffb7846);
  EXPECT_EQ(iface->methods[4].generated_ordinal64->value, 0x20d3f06c598f0cc3);
  EXPECT_EQ(iface->methods[5].generated_ordinal64->value, 0x1ce13806085dac7a);
  EXPECT_EQ(iface->methods[6].generated_ordinal64->value, 0x09e1d4b200770def);
  EXPECT_EQ(iface->methods[7].generated_ordinal64->value, 0x53df65d26411d8ee);
  EXPECT_EQ(iface->methods[8].generated_ordinal64->value, 0x690c3617405590c7);
  EXPECT_EQ(iface->methods[9].generated_ordinal64->value, 0x4ff9ef5fb170f550);
  EXPECT_EQ(iface->methods[10].generated_ordinal64->value, 0x1542d4c21d8a6c00);
  EXPECT_EQ(iface->methods[11].generated_ordinal64->value, 0x564e9e47f7418e0f);
  EXPECT_EQ(iface->methods[12].generated_ordinal64->value, 0x29681e66f3506231);
  EXPECT_EQ(iface->methods[13].generated_ordinal64->value, 0x5ee63b26268f7760);
  EXPECT_EQ(iface->methods[14].generated_ordinal64->value, 0x256950edf00aac63);
  EXPECT_EQ(iface->methods[15].generated_ordinal64->value, 0x6b21c0ff1aa02896);
  EXPECT_EQ(iface->methods[16].generated_ordinal64->value, 0x5a54f3dca00089e9);
  EXPECT_EQ(iface->methods[17].generated_ordinal64->value, 0x772476706fa4be0e);
  EXPECT_EQ(iface->methods[18].generated_ordinal64->value, 0x294e338bf71a773b);
  EXPECT_EQ(iface->methods[19].generated_ordinal64->value, 0x5a6aa228cfb68d16);
  EXPECT_EQ(iface->methods[20].generated_ordinal64->value, 0x55a09c6b033f3f98);
  EXPECT_EQ(iface->methods[21].generated_ordinal64->value, 0x1192d5b856d22cd8);
  EXPECT_EQ(iface->methods[22].generated_ordinal64->value, 0x2e68bdea28f9ce7b);
  EXPECT_EQ(iface->methods[23].generated_ordinal64->value, 0x4c8ebf26900e4451);
  EXPECT_EQ(iface->methods[24].generated_ordinal64->value, 0x3df0dbe9378c4fd3);
  EXPECT_EQ(iface->methods[25].generated_ordinal64->value, 0x087268657bb0cad1);
  EXPECT_EQ(iface->methods[26].generated_ordinal64->value, 0x0aee6ad161a90ae1);
  EXPECT_EQ(iface->methods[27].generated_ordinal64->value, 0x44e6f2282baf727a);
  EXPECT_EQ(iface->methods[28].generated_ordinal64->value, 0x3e8984f57ab5830d);
  EXPECT_EQ(iface->methods[29].generated_ordinal64->value, 0x696f9f73a5cabd21);
  EXPECT_EQ(iface->methods[30].generated_ordinal64->value, 0x327d7b0d2389e054);
  EXPECT_EQ(iface->methods[31].generated_ordinal64->value, 0x54fd307bb5bfab2d);
}

TEST(OrdinalsTests, GoodHackToRenameFuchsiaIoToFuchsiaIoOne) {
  TestLibrary library_io(R"FIDL(library fuchsia.io;

protocol SomeProtocol {
    SomeMethod();
};
)FIDL");
  ASSERT_COMPILED(library_io);

  TestLibrary library_io_one(R"FIDL(library fuchsia.io1;

protocol SomeProtocol {
    SomeMethod();
};
)FIDL");
  ASSERT_COMPILED(library_io_one);

  const fidl::flat::Protocol* io_protocol = library_io.LookupProtocol("SomeProtocol");
  uint64_t io_hash64 = io_protocol->methods[0].generated_ordinal64->value;

  const fidl::flat::Protocol* io_one_protocol = library_io_one.LookupProtocol("SomeProtocol");
  uint64_t io_one_hash64 = io_one_protocol->methods[0].generated_ordinal64->value;

  ASSERT_EQ(io_hash64, io_one_hash64);
}

}  // namespace

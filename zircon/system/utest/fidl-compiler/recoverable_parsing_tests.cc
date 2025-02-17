// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zxtest/zxtest.h>

#include "error_test.h"
#include "fidl/diagnostics.h"
#include "test_library.h"

namespace {

TEST(RecoverableParsingTests, BadRecoverAtEndOfFile) {
  TestLibrary library(R"FIDL(
library example;

type Enum = enum {
    ONE;          // First error
};

type Bits = bits {
    CONSTANT = ;  // Second error
};
)FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrUnexpectedTokenOfKind,
                                      fidl::ErrUnexpectedToken);
}

TEST(RecoverableParsingTests, BadRecoverAtEndOfDecl) {
  TestLibrary library(R"FIDL(
library example;

type Enum = enum {
    VARIANT = 0;
    MISSING_EQUALS 5;
};

type Union = union {
    1: string_value string;
    2 missing_colon uint16;
};

type Struct = struct {
    value string;
};
)FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrUnexpectedTokenOfKind,
                                      fidl::ErrUnexpectedTokenOfKind);
}

TEST(RecoverableParsingTests, BadRecoverAtEndOfMember) {
  TestLibrary library(R"FIDL(
library example;

type SettingType = enum {
    UNKNOWN = 0;
    TIME_ZONE = 1;
    CONNECTIVITY 2;                    // Error: missing equals
};

type SettingData = union {
    1: string_value string;
    2 time_zone_value ConnectedState;  // Error: missing colon
    /// Unattached doc comment.        // erroneous doc comment is skipped during recovery
};

type LoginOverride = {                 // Error: missing keyword
    NONE = 0;
    AUTH.PROVIDER = 2,                 // Error: '.' in identifier
};

type AccountSettings = table {
    1: mo.de LoginOverride;            // Error: '.' in identifier
    3: setting OtherSetting;
};

type TimeZoneInfo = struct {
    current TimeZone:optional;
    available vector<<TimeZone>;       // Error: extra <
};

type TimeZone = struct {
    id string;
    name string;
    region vector<string>;
};
  )FIDL");
  EXPECT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 6);
  ASSERT_ERR(errors[0], fidl::ErrUnexpectedTokenOfKind);
  ASSERT_ERR(errors[1], fidl::ErrUnexpectedTokenOfKind);
  ASSERT_ERR(errors[2], fidl::ErrMissingOrdinalBeforeType);
  // NOTE(fxbug.dev/72924): In the new syntax this is a parse error instead of
  // ErrExpectedDeclaration, which no longer applies in the new syntax.
  ASSERT_ERR(errors[3], fidl::ErrUnexpectedTokenOfKind);
  ASSERT_ERR(errors[4], fidl::ErrUnexpectedTokenOfKind);
  // NOTE(fxbug.dev/72924): The more specific ErrExpectedOrdinalOrCloseBrace
  // isn't returned in the new syntax. It doesn't seem all that useful anyway,
  // since we also get an ErrUnexpectedTokenOfKind
  ASSERT_ERR(errors[5], fidl::ErrUnexpectedTokenOfKind);
}

TEST(RecoverableParsingTests, BadDoNotCompileAfterParsingFails) {
  TestLibrary library(R"FIDL(
library example;

const compound.identifier uint8 = 0;  // Syntax error

type NameCollision = struct {};
type NameCollision = struct {};       // This name collision error will not be
                                      // reported, because if parsing fails
                                      // compilation is skipped
  )FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrUnexpectedTokenOfKind);
}

TEST(RecoverableParsingTests, BadRecoverToNextBitsMember) {
  TestLibrary library(R"FIDL(
library example;

type Bits = bits {
    ONE 0x1;      // First error
    TWO = 0x2;
    FOUR = 0x4    // Second error
    EIGHT = 0x8;
};
)FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrUnexpectedTokenOfKind,
                                      fidl::ErrUnexpectedTokenOfKind);
}

TEST(RecoverableParsingTests, BadRecoverToNextEnumMember) {
  TestLibrary library(R"FIDL(
library example;

type Enum = enum {
    ONE 1;      // First error
    TWO = 2;
    THREE = 3   // Second error
    FOUR = 4;
};
)FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrUnexpectedTokenOfKind,
                                      fidl::ErrUnexpectedTokenOfKind);
}

TEST(RecoverableParsingTests, BadRecoverToNextProtocolMember) {
  TestLibrary library(R"FIDL(
library example;

protocol P {
    compose A B;                                 // 2 Errors (on 'B', ';')
    MethodWithoutSemicolon()
    ValidMethod();                               // Error (expecting ';')
    -> Event(struct { TypeWithoutParamName; });  // Error
    MissingParen server_end:Protocol protocol);  // Error
    -> Event(struct { missing_paren T };         // 2 Errors (on '}', ';')
    ValidMethod();
    Method() -> (struct { num uint16; }) error;  // Error
};
)FIDL");
  EXPECT_FALSE(library.Compile());
  const auto& errors = library.errors();
  // NOTE(fxbug.dev/72924): the difference in errors is due to the change in
  // test input (for the TypeWithoutParams and MissingParen cases) rather than
  // any real behavior change
  ASSERT_EQ(errors.size(), 8);
  ASSERT_ERR(errors[0], fidl::ErrUnexpectedTokenOfKind);
  ASSERT_ERR(errors[1], fidl::ErrUnrecognizedProtocolMember);
  ASSERT_ERR(errors[2], fidl::ErrUnexpectedTokenOfKind);
  ASSERT_ERR(errors[3], fidl::ErrUnexpectedTokenOfKind);
  ASSERT_ERR(errors[4], fidl::ErrUnrecognizedProtocolMember);
  ASSERT_ERR(errors[5], fidl::ErrUnexpectedTokenOfKind);
  ASSERT_ERR(errors[6], fidl::ErrUnexpectedTokenOfKind);
  ASSERT_ERR(errors[7], fidl::ErrUnexpectedTokenOfKind);
}

TEST(ParsingTests, BadRecoverableParamListParsing) {
  TestLibrary library("example.fidl", R"FIDL(
library example;

protocol Example {
  Method(/// Doc comment
      struct { b bool; }) -> (/// Doc comment
      struct { b bool; });
};
)FIDL");

  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrDocCommentOnParameters,
                                      fidl::ErrDocCommentOnParameters);
}

TEST(RecoverableParsingTests, BadRecoverToNextServiceMember) {
  TestLibrary library(R"FIDL(
library example;

protocol P {};
protocol Q {};
protocol R {};

service Service {
  p P extra_token; // First error
  q Q              // Second error
  r R;
};
)FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrUnexpectedTokenOfKind,
                                      fidl::ErrUnexpectedTokenOfKind);
}

TEST(RecoverableParsingTests, BadRecoverToNextStructMember) {
  TestLibrary library(R"FIDL(
library example;

type Struct = struct {
    string_value string extra_token; // Error
    uint_value uint8;
    vector_value vector<handle>      // Error
    int_value int32;
};
)FIDL");
  EXPECT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 3);
  ASSERT_ERR(errors[0], fidl::ErrUnexpectedTokenOfKind);
  ASSERT_ERR(errors[1], fidl::ErrUnexpectedTokenOfKind);
  ASSERT_ERR(errors[2], fidl::ErrUnexpectedTokenOfKind);
}

TEST(RecoverableParsingTests, BadRecoverToNextTableMember) {
  TestLibrary library(R"FIDL(
library example;

type Table = table {
    1: string_value string              // Error
    2: uint_value uint8;
    3: value_with space vector<handle>; // Error
    4: int_value int32;
};
)FIDL");
  EXPECT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 3);
  ASSERT_ERR(errors[0], fidl::ErrUnexpectedTokenOfKind);
  ASSERT_ERR(errors[1], fidl::ErrUnexpectedTokenOfKind);
  // NOTE(fxbug.dev/72924): the difference here is just due to the type/member
  // reordering, not a behavior change
  ASSERT_ERR(errors[2], fidl::ErrMissingOrdinalBeforeType);
}

TEST(RecoverableParsingTests, BadRecoverToNextUnionMember) {
  TestLibrary library(R"FIDL(
library example;

type Union = union {
    1 missing_colon string;     // First error
    3: uint_value uint8;
    4: missing_semicolon string // Second error
    5: int_value int16;
};
)FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrUnexpectedTokenOfKind,
                                      fidl::ErrUnexpectedTokenOfKind);
}

TEST(RecoverableParsingTests, BadRecoverFinalMemberMissingSemicolon) {
  TestLibrary library(R"FIDL(
library example;

type Struct = struct {
    uint_value uint8;
    foo string // First error
};

// Recovered back to top-level parsing.
type Good = struct {};

extra_token // Second error
)FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrUnexpectedTokenOfKind,
                                      fidl::ErrExpectedDeclaration);
}

TEST(RecoverableParsingTests, BadRecoverFinalMemberMissingNameAndSemicolon) {
  TestLibrary library(R"FIDL(
library example;

type Struct = struct {
    uint_value uint8;
    string_value }; // First error

// Does not recover back to top-level parsing. End the struct.
};

// Back to top-level parsing.
type Good = struct {};

extra_token // Second error
)FIDL");
  ASSERT_ERRORED_TWICE_DURING_COMPILE(library, fidl::ErrUnexpectedTokenOfKind,
                                      fidl::ErrExpectedDeclaration);
}

}  // namespace

// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fbl/string.h>
#include <fbl/string_printf.h>
#include <fbl/vector.h>
#include <unittest/unittest.h>
#include <zircon/types.h>

#include "util.h"

namespace {

struct SplitStringTestCase {
  const char* input;
  char delimiter;
};

// Test the SplitString util individually.
bool TestSplitString() {
  BEGIN_TEST;

  // Makeshift parametrized test.
  struct {
    const char* input;
    char delimiter;
  } cases[] = {
      {.input = "", .delimiter = ','},        {.input = "abcd", .delimiter = ','},
      {.input = "a,b c,d", .delimiter = ','}, {.input = "a,b c,d", .delimiter = ' '},
      {.input = "::a:", .delimiter = ':'},
  };
  fbl::Vector<fbl::String> expected[] = {
      {}, {"abcd"}, {"a", "b c", "d"}, {"a,b", "c,d"}, {"", "", "a", ""},
  };
  for (size_t n = 0; n < sizeof(cases) / sizeof(*cases); ++n) {
    fbl::String case_msg = fbl::StringPrintf("Test Case %zu", n);
    auto result = bootsvc::SplitString(cases[n].input, cases[n].delimiter);

    // We use EXPECT_EQ rather than ASSERT_EQ so that we can continue with
    // other test cases if one fails.
    EXPECT_EQ(result.size(), expected[n].size(), case_msg.c_str());
    if (result.size() == expected[n].size()) {
      for (size_t i = 0; i < result.size(); ++i) {
        EXPECT_STR_EQ(result[i].c_str(), expected[n][i].c_str(), case_msg.c_str());
      }
    }
  }

  END_TEST;
}

// Make sure that we can parse boot args from a configuration string
bool TestParseBootArgs() {
  BEGIN_TEST;

  const char config1[] = R"(
# comment
key
key=value
=value
)";

  // Parse a valid config.
  fbl::Vector<char> buf;
  zx_status_t status = bootsvc::ParseBootArgs(config1, &buf);
  ASSERT_EQ(ZX_OK, status);

  const char expected[] = "key\0key=value";
  auto actual = reinterpret_cast<const uint8_t*>(buf.get());
  ASSERT_BYTES_EQ(reinterpret_cast<const uint8_t*>(expected), actual, buf.size(), "");

  // Parse an invalid config.
  const char config2[] = "k ey=value";
  status = bootsvc::ParseBootArgs(config2, &buf);
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, status);

  END_TEST;
}

}  // namespace

BEGIN_TEST_CASE(bootsvc_unit_tests)
RUN_TEST(TestParseBootArgs)
RUN_TEST(TestSplitString)
END_TEST_CASE(bootsvc_unit_tests)

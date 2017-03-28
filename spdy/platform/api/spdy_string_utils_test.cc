// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/platform/api/spdy_string_utils.h"

#include <cstdint>

#include "net/spdy/platform/api/spdy_string_piece.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {
namespace test {
namespace {

TEST(SpdyStrUtilsTest, StrCat) {
  // No arguments.
  EXPECT_EQ("", SpdyStrCat());

  // Single string-like argument.
  const char kFoo[] = "foo";
  const std::string string_foo(kFoo);
  const SpdyStringPiece stringpiece_foo(string_foo);
  EXPECT_EQ("foo", SpdyStrCat(kFoo));
  EXPECT_EQ("foo", SpdyStrCat(string_foo));
  EXPECT_EQ("foo", SpdyStrCat(stringpiece_foo));

  // Two string-like arguments.
  const char kBar[] = "bar";
  const SpdyStringPiece stringpiece_bar(kBar);
  const std::string string_bar(kBar);
  EXPECT_EQ("foobar", SpdyStrCat(kFoo, kBar));
  EXPECT_EQ("foobar", SpdyStrCat(kFoo, string_bar));
  EXPECT_EQ("foobar", SpdyStrCat(kFoo, stringpiece_bar));
  EXPECT_EQ("foobar", SpdyStrCat(string_foo, kBar));
  EXPECT_EQ("foobar", SpdyStrCat(string_foo, string_bar));
  EXPECT_EQ("foobar", SpdyStrCat(string_foo, stringpiece_bar));
  EXPECT_EQ("foobar", SpdyStrCat(stringpiece_foo, kBar));
  EXPECT_EQ("foobar", SpdyStrCat(stringpiece_foo, string_bar));
  EXPECT_EQ("foobar", SpdyStrCat(stringpiece_foo, stringpiece_bar));

  // Many-many arguments.
  EXPECT_EQ(
      "foobarbazquxquuxquuzcorgegraultgarplywaldofredplughxyzzythud",
      SpdyStrCat("foo", "bar", "baz", "qux", "quux", "quuz", "corge", "grault",
                 "garply", "waldo", "fred", "plugh", "xyzzy", "thud"));

  // Numerical arguments.
  const int16_t i = 1;
  const uint64_t u = 8;
  const double d = 3.1415;

  EXPECT_EQ("1 8", SpdyStrCat(i, " ", u));
  EXPECT_EQ("3.14151181", SpdyStrCat(d, i, i, u, i));
  EXPECT_EQ("i: 1, u: 8, d: 3.1415",
            SpdyStrCat("i: ", i, ", u: ", u, ", d: ", d));

  // Boolean arguments.
  const bool t = true;
  const bool f = false;

  EXPECT_EQ("1", SpdyStrCat(t));
  EXPECT_EQ("0", SpdyStrCat(f));
  EXPECT_EQ("0110", SpdyStrCat(f, t, t, f));

  // Mixed string-like, numerical, and Boolean arguments.
  EXPECT_EQ("foo1foo081bar3.14151",
            SpdyStrCat(kFoo, i, string_foo, f, u, t, stringpiece_bar, d, t));
  EXPECT_EQ("3.141511bar18bar13.14150",
            SpdyStrCat(d, t, t, string_bar, i, u, kBar, t, d, f));
}

TEST(SpdyStrUtilsTest, StrAppend) {
  // No arguments on empty string.
  std::string output;
  SpdyStrAppend(&output);
  EXPECT_TRUE(output.empty());

  // Single string-like argument.
  const char kFoo[] = "foo";
  const std::string string_foo(kFoo);
  const SpdyStringPiece stringpiece_foo(string_foo);
  SpdyStrAppend(&output, kFoo);
  EXPECT_EQ("foo", output);
  SpdyStrAppend(&output, string_foo);
  EXPECT_EQ("foofoo", output);
  SpdyStrAppend(&output, stringpiece_foo);
  EXPECT_EQ("foofoofoo", output);

  // No arguments on non-empty string.
  SpdyStrAppend(&output);
  EXPECT_EQ("foofoofoo", output);

  output.clear();

  // Two string-like arguments.
  const char kBar[] = "bar";
  const SpdyStringPiece stringpiece_bar(kBar);
  const std::string string_bar(kBar);
  SpdyStrAppend(&output, kFoo, kBar);
  EXPECT_EQ("foobar", output);
  SpdyStrAppend(&output, kFoo, string_bar);
  EXPECT_EQ("foobarfoobar", output);
  SpdyStrAppend(&output, kFoo, stringpiece_bar);
  EXPECT_EQ("foobarfoobarfoobar", output);
  SpdyStrAppend(&output, string_foo, kBar);
  EXPECT_EQ("foobarfoobarfoobarfoobar", output);

  output.clear();

  SpdyStrAppend(&output, string_foo, string_bar);
  EXPECT_EQ("foobar", output);
  SpdyStrAppend(&output, string_foo, stringpiece_bar);
  EXPECT_EQ("foobarfoobar", output);
  SpdyStrAppend(&output, stringpiece_foo, kBar);
  EXPECT_EQ("foobarfoobarfoobar", output);
  SpdyStrAppend(&output, stringpiece_foo, string_bar);
  EXPECT_EQ("foobarfoobarfoobarfoobar", output);

  output.clear();

  SpdyStrAppend(&output, stringpiece_foo, stringpiece_bar);
  EXPECT_EQ("foobar", output);

  // Many-many arguments.
  SpdyStrAppend(&output, "foo", "bar", "baz", "qux", "quux", "quuz", "corge",
                "grault", "garply", "waldo", "fred", "plugh", "xyzzy", "thud");
  EXPECT_EQ(
      "foobarfoobarbazquxquuxquuzcorgegraultgarplywaldofredplughxyzzythud",
      output);

  output.clear();

  // Numerical arguments.
  const int16_t i = 1;
  const uint64_t u = 8;
  const double d = 3.1415;

  SpdyStrAppend(&output, i, " ", u);
  EXPECT_EQ("1 8", output);
  SpdyStrAppend(&output, d, i, i, u, i);
  EXPECT_EQ("1 83.14151181", output);
  SpdyStrAppend(&output, "i: ", i, ", u: ", u, ", d: ", d);
  EXPECT_EQ("1 83.14151181i: 1, u: 8, d: 3.1415", output);

  output.clear();

  // Boolean arguments.
  const bool t = true;
  const bool f = false;

  SpdyStrAppend(&output, t);
  EXPECT_EQ("1", output);
  SpdyStrAppend(&output, f);
  EXPECT_EQ("10", output);
  SpdyStrAppend(&output, f, t, t, f);
  EXPECT_EQ("100110", output);

  output.clear();

  // Mixed string-like, numerical, and Boolean arguments.
  SpdyStrAppend(&output, kFoo, i, string_foo, f, u, t, stringpiece_bar, d, t);
  EXPECT_EQ("foo1foo081bar3.14151", output);
  SpdyStrAppend(&output, d, t, t, string_bar, i, u, kBar, t, d, f);
  EXPECT_EQ("foo1foo081bar3.141513.141511bar18bar13.14150", output);
}

TEST(SpdyStrUtilsTest, StringPrintf) {
  EXPECT_EQ("", SpdyStringPrintf("%s", ""));
  EXPECT_EQ("foobar", SpdyStringPrintf("%sbar", "foo"));
  EXPECT_EQ("foobar", SpdyStringPrintf("%s%s", "foo", "bar"));
  EXPECT_EQ("foo: 1, bar: 2.0", SpdyStringPrintf("foo: %d, bar: %.1f", 1, 2.0));
}

}  // namespace
}  // namespace test
}  // namespace net

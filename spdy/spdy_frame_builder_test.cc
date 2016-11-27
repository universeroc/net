// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/spdy_frame_builder.h"

#include "net/spdy/spdy_framer.h"
#include "net/spdy/spdy_protocol.h"
#include "testing/platform_test.h"

namespace net {

TEST(SpdyFrameBuilderTest, GetWritableBuffer) {
  const size_t builder_size = 10;
  SpdyFrameBuilder builder(builder_size, HTTP2);
  char* writable_buffer = builder.GetWritableBuffer(builder_size);
  memset(writable_buffer, ~1, builder_size);
  EXPECT_TRUE(builder.Seek(builder_size));
  SpdySerializedFrame frame(builder.take());
  char expected[builder_size];
  memset(expected, ~1, builder_size);
  EXPECT_EQ(base::StringPiece(expected, builder_size),
            base::StringPiece(frame.data(), builder_size));
}

TEST(SpdyFrameBuilderTest, RewriteLength) {
  // Create an empty SETTINGS frame both via framer and manually via builder.
  // The one created via builder is initially given the incorrect length, but
  // then is corrected via RewriteLength().
  SpdyFramer framer(HTTP2);
  SpdySettingsIR settings_ir;
  SpdySerializedFrame expected(framer.SerializeSettings(settings_ir));
  SpdyFrameBuilder builder(expected.size() + 1, HTTP2);
  builder.BeginNewFrame(framer, SETTINGS, 0, 0);
  EXPECT_TRUE(builder.GetWritableBuffer(1) != NULL);
  builder.RewriteLength(framer);
  SpdySerializedFrame built(builder.take());
  EXPECT_EQ(base::StringPiece(expected.data(), expected.size()),
            base::StringPiece(built.data(), expected.size()));
}

TEST(SpdyFrameBuilderTest, OverwriteFlags) {
  // Create a HEADERS frame both via framer and manually via builder with
  // different flags set, then make them match using OverwriteFlags().
  SpdyFramer framer(HTTP2);
  SpdyHeadersIR headers_ir(1);
  SpdySerializedFrame expected(framer.SerializeHeaders(headers_ir));
  SpdyFrameBuilder builder(expected.size(), HTTP2);
  builder.BeginNewFrame(framer, HEADERS, 0, 1);
  builder.OverwriteFlags(framer, HEADERS_FLAG_END_HEADERS);
  SpdySerializedFrame built(builder.take());
  EXPECT_EQ(base::StringPiece(expected.data(), expected.size()),
            base::StringPiece(built.data(), built.size()));
}
}  // namespace net

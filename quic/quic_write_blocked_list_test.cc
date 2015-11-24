// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
#include "net/quic/quic_write_blocked_list.h"

#include "net/quic/test_tools/quic_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

using net::kLowestPriority;
using net::kHighestPriority;

using net::kLowestPriority;
using net::kHighestPriority;

namespace net {
namespace test {
namespace {

TEST(QuicWriteBlockedListTest, PriorityOrder) {
  QuicWriteBlockedList write_blocked_list;

  // Mark streams blocked in roughly reverse priority order, and
  // verify that streams are sorted.
  write_blocked_list.AddStream(40, net::kLowestPriority);
  write_blocked_list.AddStream(23, net::kHighestPriority);
  write_blocked_list.AddStream(17, net::kHighestPriority);
  write_blocked_list.AddStream(kHeadersStreamId, net::kHighestPriority);
  write_blocked_list.AddStream(kCryptoStreamId, net::kHighestPriority);

  EXPECT_EQ(5u, write_blocked_list.NumBlockedStreams());
  EXPECT_TRUE(write_blocked_list.HasWriteBlockedCryptoOrHeadersStream());
  EXPECT_TRUE(write_blocked_list.HasWriteBlockedDataStreams());
  // The Crypto stream is highest priority.
  EXPECT_EQ(kCryptoStreamId, write_blocked_list.PopFront());
  // Followed by the Headers stream.
  EXPECT_EQ(kHeadersStreamId, write_blocked_list.PopFront());
  // Streams with same priority are popped in the order they were inserted.
  EXPECT_EQ(23u, write_blocked_list.PopFront());
  EXPECT_EQ(17u, write_blocked_list.PopFront());
  // Low priority stream appears last.
  EXPECT_EQ(40u, write_blocked_list.PopFront());

  EXPECT_EQ(0u, write_blocked_list.NumBlockedStreams());
  EXPECT_FALSE(write_blocked_list.HasWriteBlockedCryptoOrHeadersStream());
  EXPECT_FALSE(write_blocked_list.HasWriteBlockedDataStreams());
}

TEST(QuicWriteBlockedListTest, CryptoStream) {
  QuicWriteBlockedList write_blocked_list;
  write_blocked_list.AddStream(kCryptoStreamId, net::kHighestPriority);

  EXPECT_EQ(1u, write_blocked_list.NumBlockedStreams());
  EXPECT_TRUE(write_blocked_list.HasWriteBlockedCryptoOrHeadersStream());
  EXPECT_EQ(kCryptoStreamId, write_blocked_list.PopFront());
  EXPECT_EQ(0u, write_blocked_list.NumBlockedStreams());
  EXPECT_FALSE(write_blocked_list.HasWriteBlockedCryptoOrHeadersStream());
}

TEST(QuicWriteBlockedListTest, HeadersStream) {
  QuicWriteBlockedList write_blocked_list;
  write_blocked_list.AddStream(kHeadersStreamId, net::kHighestPriority);

  EXPECT_EQ(1u, write_blocked_list.NumBlockedStreams());
  EXPECT_TRUE(write_blocked_list.HasWriteBlockedCryptoOrHeadersStream());
  EXPECT_EQ(kHeadersStreamId, write_blocked_list.PopFront());
  EXPECT_EQ(0u, write_blocked_list.NumBlockedStreams());
  EXPECT_FALSE(write_blocked_list.HasWriteBlockedCryptoOrHeadersStream());
}

TEST(QuicWriteBlockedListTest, VerifyHeadersStream) {
  QuicWriteBlockedList write_blocked_list;
  write_blocked_list.AddStream(5, net::kHighestPriority);
  write_blocked_list.AddStream(kHeadersStreamId, net::kHighestPriority);

  EXPECT_EQ(2u, write_blocked_list.NumBlockedStreams());
  EXPECT_TRUE(write_blocked_list.HasWriteBlockedCryptoOrHeadersStream());
  EXPECT_TRUE(write_blocked_list.HasWriteBlockedDataStreams());
  // In newer QUIC versions, there is a headers stream which is
  // higher priority than data streams.
  EXPECT_EQ(kHeadersStreamId, write_blocked_list.PopFront());
  EXPECT_EQ(5u, write_blocked_list.PopFront());
  EXPECT_EQ(0u, write_blocked_list.NumBlockedStreams());
  EXPECT_FALSE(write_blocked_list.HasWriteBlockedCryptoOrHeadersStream());
  EXPECT_FALSE(write_blocked_list.HasWriteBlockedDataStreams());
}

TEST(QuicWriteBlockedListTest, NoDuplicateEntries) {
  // Test that QuicWriteBlockedList doesn't allow duplicate entries.
  QuicWriteBlockedList write_blocked_list;

  // Try to add a stream to the write blocked list multiple times at the same
  // priority.
  const QuicStreamId kBlockedId = kClientDataStreamId1;
  write_blocked_list.AddStream(kBlockedId, net::kHighestPriority);
  write_blocked_list.AddStream(kBlockedId, net::kHighestPriority);
  write_blocked_list.AddStream(kBlockedId, net::kHighestPriority);

  // This should only result in one blocked stream being added.
  EXPECT_EQ(1u, write_blocked_list.NumBlockedStreams());
  EXPECT_TRUE(write_blocked_list.HasWriteBlockedDataStreams());

  // There should only be one stream to pop off the front.
  EXPECT_EQ(kBlockedId, write_blocked_list.PopFront());
  EXPECT_EQ(0u, write_blocked_list.NumBlockedStreams());
  EXPECT_FALSE(write_blocked_list.HasWriteBlockedDataStreams());
}

TEST(QuicWriteBlockedListTest, BatchingWrites) {
  FLAGS_quic_batch_writes = true;
  QuicWriteBlockedList write_blocked_list;

  const QuicStreamId id1 = kClientDataStreamId1;
  const QuicStreamId id2 = kClientDataStreamId2;

  write_blocked_list.AddStream(id1, net::kLowestPriority);
  write_blocked_list.AddStream(id2, net::kLowestPriority);
  EXPECT_EQ(2u, write_blocked_list.NumBlockedStreams());

  // The first stream we push back should stay at the front until 16k is
  // written.
  EXPECT_EQ(id1, write_blocked_list.PopFront());
  write_blocked_list.AddStream(id1, net::kHighestPriority);
  write_blocked_list.UpdateBytesForStream(id1, 15999);
  write_blocked_list.AddStream(id1, net::kLowestPriority);
  EXPECT_EQ(2u, write_blocked_list.NumBlockedStreams());
  EXPECT_EQ(id1, write_blocked_list.PopFront());

  // Once 16k is written the first stream will cede to the next.
  write_blocked_list.AddStream(id1, net::kHighestPriority);
  write_blocked_list.UpdateBytesForStream(id1, 1);
  write_blocked_list.AddStream(id1, net::kLowestPriority);
  EXPECT_EQ(2u, write_blocked_list.NumBlockedStreams());
  EXPECT_EQ(id2, write_blocked_list.PopFront());

  // Set the new stream to have written all but one byte.
  write_blocked_list.AddStream(id2, net::kHighestPriority);
  write_blocked_list.UpdateBytesForStream(id2, 15999);
  write_blocked_list.AddStream(id2, net::kLowestPriority);
  EXPECT_EQ(2u, write_blocked_list.NumBlockedStreams());

  // Ensure higher priority streams are popped first.
  const QuicStreamId id3 = kClientDataStreamId2 + 2;
  write_blocked_list.AddStream(id3, net::kHighestPriority);
  EXPECT_EQ(id3, write_blocked_list.PopFront());

  // Higher priority streams will always be popped first, even if using their
  // byte quota
  write_blocked_list.UpdateBytesForStream(id3, 20000);
  write_blocked_list.AddStream(id3, net::kHighestPriority);
  EXPECT_EQ(id3, write_blocked_list.PopFront());

  // Once the higher priority stream is out of the way, id2 will resume its 16k
  // write, with only 1 byte remaining of its guaranteed write allocation.
  EXPECT_EQ(id2, write_blocked_list.PopFront());
  write_blocked_list.AddStream(id2, net::kHighestPriority);
  write_blocked_list.UpdateBytesForStream(id2, 1);
  write_blocked_list.AddStream(id2, net::kLowestPriority);
  EXPECT_EQ(2u, write_blocked_list.NumBlockedStreams());
  EXPECT_EQ(id1, write_blocked_list.PopFront());
}

}  // namespace
}  // namespace test
}  // namespace net

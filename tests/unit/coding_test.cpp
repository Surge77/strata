#include "util/coding.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using strata::Slice;

TEST(Fixed, IsLittleEndianOnDiskRegardlessOfHost) {
  // Pinned as explicit bytes rather than a round trip: a round trip would still
  // pass if both ends switched to host order together, and the on-disk byte
  // order is a format guarantee.
  std::string encoded;
  strata::PutFixed32(&encoded, 0x01020304U);
  ASSERT_EQ(encoded.size(), 4U);
  EXPECT_EQ(static_cast<std::uint8_t>(encoded[0]), 0x04U);
  EXPECT_EQ(static_cast<std::uint8_t>(encoded[3]), 0x01U);

  std::string wide;
  strata::PutFixed64(&wide, 0x0102030405060708ULL);
  ASSERT_EQ(wide.size(), 8U);
  EXPECT_EQ(static_cast<std::uint8_t>(wide[0]), 0x08U);
  EXPECT_EQ(static_cast<std::uint8_t>(wide[7]), 0x01U);
}

TEST(Fixed, RoundTripsBoundaryValues) {
  for (const std::uint32_t value : {0U, 1U, 0x7FFFFFFFU, 0x80000000U, 0xFFFFFFFFU}) {
    std::string encoded;
    strata::PutFixed32(&encoded, value);
    EXPECT_EQ(strata::DecodeFixed32(encoded.data()), value);
  }
  const std::array<std::uint64_t, 5> wide_values{0ULL, 1ULL, 0x7FFFFFFFFFFFFFFFULL,
                                                 0x8000000000000000ULL,
                                                 std::numeric_limits<std::uint64_t>::max()};
  for (const std::uint64_t value : wide_values) {
    std::string encoded;
    strata::PutFixed64(&encoded, value);
    EXPECT_EQ(strata::DecodeFixed64(encoded.data()), value);
  }
}

TEST(Varint, UsesOneByteBelowTheContinuationThreshold) {
  EXPECT_EQ(strata::VarintLength(0), 1);
  EXPECT_EQ(strata::VarintLength(127), 1);
  EXPECT_EQ(strata::VarintLength(128), 2);
  EXPECT_EQ(strata::VarintLength(std::numeric_limits<std::uint64_t>::max()), 10);
}

TEST(Varint, RoundTripsBoundaryValues32) {
  const std::vector<std::uint32_t> values{0U,     1U,     127U,      128U,
                                          16383U, 16384U, 0xFFFFFFU, 0xFFFFFFFFU};
  std::string encoded;
  for (const std::uint32_t value : values) {
    strata::PutVarint32(&encoded, value);
  }

  Slice cursor(encoded);
  for (const std::uint32_t expected : values) {
    std::uint32_t decoded = 0;
    ASSERT_TRUE(strata::GetVarint32(&cursor, &decoded)) << expected;
    EXPECT_EQ(decoded, expected);
  }
  EXPECT_TRUE(cursor.empty());
}

TEST(Varint, RoundTripsBoundaryValues64) {
  const std::vector<std::uint64_t> values{
      0ULL, 1ULL, 127ULL, 128ULL, 1ULL << 35U, std::numeric_limits<std::uint64_t>::max()};
  std::string encoded;
  for (const std::uint64_t value : values) {
    strata::PutVarint64(&encoded, value);
  }

  Slice cursor(encoded);
  for (const std::uint64_t expected : values) {
    std::uint64_t decoded = 0;
    ASSERT_TRUE(strata::GetVarint64(&cursor, &decoded)) << expected;
    EXPECT_EQ(decoded, expected);
  }
  EXPECT_TRUE(cursor.empty());
}

TEST(Varint, RejectsATruncatedEncodingAtEveryOffset) {
  std::string encoded;
  strata::PutVarint64(&encoded, std::numeric_limits<std::uint64_t>::max());
  ASSERT_EQ(encoded.size(), 10U);

  for (std::size_t prefix = 0; prefix < encoded.size(); ++prefix) {
    Slice cursor(encoded.data(), prefix);
    std::uint64_t decoded = 0;
    EXPECT_FALSE(strata::GetVarint64(&cursor, &decoded)) << "prefix of " << prefix << " bytes";
  }
}

TEST(Varint, RejectsAnOverLongEncoding) {
  // Eleven bytes that all set the continuation bit: what a run of 0xFF in a
  // corrupt file looks like. Without the length bound this would shift past the
  // width of the result and keep reading.
  const std::string malformed(11, '\xFF');
  Slice cursor(malformed);
  std::uint64_t decoded = 0;
  EXPECT_FALSE(strata::GetVarint64(&cursor, &decoded));
  EXPECT_EQ(cursor.size(), malformed.size()) << "input must be left untouched on failure";
}

TEST(Varint, RejectsAThirtyTwoBitValueEncodedTooWide) {
  const std::string malformed(6, '\x80');
  Slice cursor(malformed);
  std::uint32_t decoded = 0;
  EXPECT_FALSE(strata::GetVarint32(&cursor, &decoded));
}

TEST(Varint, LeavesTheInputUntouchedWhenDecodingFails) {
  const std::string truncated(1, '\x80');
  Slice cursor(truncated);
  std::uint32_t decoded = 0;
  ASSERT_FALSE(strata::GetVarint32(&cursor, &decoded));
  EXPECT_EQ(cursor.size(), 1U);
  EXPECT_EQ(cursor.data(), truncated.data());
}

TEST(LengthPrefixed, RoundTripsIncludingEmptyAndEmbeddedNuls) {
  const std::string payload("a\0b", 3);
  std::string encoded;
  strata::PutLengthPrefixedSlice(&encoded, "");
  strata::PutLengthPrefixedSlice(&encoded, payload);
  strata::PutLengthPrefixedSlice(&encoded, "trailing");

  Slice cursor(encoded);
  Slice value;
  ASSERT_TRUE(strata::GetLengthPrefixedSlice(&cursor, &value));
  EXPECT_TRUE(value.empty());
  ASSERT_TRUE(strata::GetLengthPrefixedSlice(&cursor, &value));
  EXPECT_EQ(value, Slice(payload));
  ASSERT_TRUE(strata::GetLengthPrefixedSlice(&cursor, &value));
  EXPECT_EQ(value, "trailing");
  EXPECT_TRUE(cursor.empty());
}

TEST(LengthPrefixed, RejectsALengthLargerThanTheBuffer) {
  // The corruption that matters: a length field claiming far more bytes than
  // exist. Trusting it would mean reading, or allocating, out of bounds.
  std::string encoded;
  strata::PutVarint64(&encoded, 1ULL << 40U);
  encoded.append("short");

  Slice cursor(encoded);
  Slice value;
  EXPECT_FALSE(strata::GetLengthPrefixedSlice(&cursor, &value));
  EXPECT_EQ(cursor.size(), encoded.size());
}

TEST(LengthPrefixed, RejectsALengthOffByOne) {
  std::string encoded;
  strata::PutVarint64(&encoded, 6);
  encoded.append("12345");

  Slice cursor(encoded);
  Slice value;
  EXPECT_FALSE(strata::GetLengthPrefixedSlice(&cursor, &value));
}

TEST(LengthPrefixed, RejectsAMissingLengthField) {
  Slice cursor;
  Slice value;
  EXPECT_FALSE(strata::GetLengthPrefixedSlice(&cursor, &value));
}

}  // namespace

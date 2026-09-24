#include "strata/slice.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using strata::AsBytes;
using strata::AsSlice;
using strata::SharedPrefixLength;
using strata::Slice;

TEST(Slice, PreservesEmbeddedNulBytes) {
  const std::string key("a\0b", 3);
  const Slice slice(key);
  EXPECT_EQ(slice.size(), 3U);
  EXPECT_EQ(slice[1], '\0');
}

TEST(Slice, ComparesBytesAsUnsigned) {
  // 0x80 is negative as a signed char. If comparison used signed chars this
  // would order "\x80" before "\x01", which would corrupt key ordering on
  // every non-ASCII key the engine ever stores.
  const std::string high("\x80", 1);
  const std::string low("\x01", 1);
  EXPECT_LT(Slice(low), Slice(high));
}

TEST(Slice, EmptyIsDistinctFromSingleNul) {
  EXPECT_NE(Slice(), Slice(std::string("\0", 1)));
}

TEST(SharedPrefix, ReturnsZeroWhenFirstByteDiffers) {
  EXPECT_EQ(SharedPrefixLength("abc", "xbc"), 0U);
}

TEST(SharedPrefix, StopsAtTheShorterInput) {
  EXPECT_EQ(SharedPrefixLength("user:", "user:1234"), 5U);
  EXPECT_EQ(SharedPrefixLength("user:1234", "user:"), 5U);
}

TEST(SharedPrefix, HandlesIdenticalAndEmptyInputs) {
  EXPECT_EQ(SharedPrefixLength("same", "same"), 4U);
  EXPECT_EQ(SharedPrefixLength("", "anything"), 0U);
  EXPECT_EQ(SharedPrefixLength("", ""), 0U);
}

TEST(SharedPrefix, CountsBytesNotCharacters) {
  const std::string a("pre\0fix", 7);
  const std::string b("pre\0fox", 7);
  EXPECT_EQ(SharedPrefixLength(a, b), 5U);
}

TEST(ByteConversion, RoundTripsThroughSpanWithoutCopying) {
  const std::string data("\x00\xff\x7f", 3);
  const Slice original(data);

  const std::span<const std::byte> bytes = AsBytes(original);
  ASSERT_EQ(bytes.size(), 3U);
  EXPECT_EQ(static_cast<unsigned>(bytes[1]), 0xffU);

  const Slice restored = AsSlice(bytes);
  EXPECT_EQ(restored, original);
  EXPECT_EQ(restored.data(), original.data());
}

TEST(ByteConversion, HandlesEmptyRanges) {
  EXPECT_TRUE(AsSlice(AsBytes(Slice{})).empty());
}

}  // namespace

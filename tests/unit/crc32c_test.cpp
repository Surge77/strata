#include "util/crc32c.hpp"

#include <array>
#include <cstdint>
#include <numeric>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace crc32c = strata::crc32c;
using strata::Slice;

std::string Repeat(char byte, std::size_t count) {
  return std::string(count, byte);
}

std::string Ascending(std::size_t count) {
  std::string data(count, '\0');
  for (std::size_t index = 0; index < count; ++index) {
    data[index] = static_cast<char>(index);
  }
  return data;
}

std::string Descending(std::size_t count) {
  std::string data(count, '\0');
  for (std::size_t index = 0; index < count; ++index) {
    data[index] = static_cast<char>(count - 1 - index);
  }
  return data;
}

// The vectors from RFC 3720 appendix B.4. These are the reason this test exists:
// a CRC implementation that is self-consistent but wrong produces files no other
// tool can validate, and the mistake is invisible until someone else reads them.
TEST(Crc32c, MatchesRfc3720Vectors) {
  EXPECT_EQ(crc32c::Value(Repeat('\x00', 32)), 0x8A9136AAU);
  EXPECT_EQ(crc32c::Value(Repeat('\xFF', 32)), 0x62A8AB43U);
  EXPECT_EQ(crc32c::Value(Ascending(32)), 0x46DD794EU);
  EXPECT_EQ(crc32c::Value(Descending(32)), 0x113FDB5CU);
}

TEST(Crc32c, MatchesTheStandardCheckValue) {
  // The check value published in the CRC catalogue for CRC-32/ISCSI.
  EXPECT_EQ(crc32c::Value("123456789"), 0xE3069283U);
}

TEST(Crc32c, EmptyInputIsZero) {
  EXPECT_EQ(crc32c::Value(""), 0U);
}

TEST(Crc32c, DetectsASingleFlippedBit) {
  std::string data = "the quick brown fox";
  const std::uint32_t original = crc32c::Value(data);
  data[7] = static_cast<char>(data[7] ^ 0x01);
  EXPECT_NE(crc32c::Value(data), original);
}

TEST(Crc32c, ExtendMatchesASingleShotOverTheJoinedInput) {
  const std::string whole = "the quick brown fox jumps over the lazy dog";
  for (std::size_t split = 0; split <= whole.size(); ++split) {
    const std::uint32_t chained =
        crc32c::Extend(crc32c::Value(Slice(whole).substr(0, split)), Slice(whole).substr(split));
    EXPECT_EQ(chained, crc32c::Value(whole)) << "split at " << split;
  }
}

TEST(Crc32c, HardwareAndPortableAgree) {
  if (!crc32c::HasHardwareSupport()) {
    GTEST_SKIP() << "no SSE 4.2 on this machine; only the portable path is in use";
  }
  // Lengths chosen to exercise the unaligned head, the 8-byte body, and the
  // tail of the hardware loop, since those are three separate code paths.
  for (std::size_t length = 0; length <= 40; ++length) {
    const std::string data = Ascending(length);
    for (std::size_t offset = 0; offset < 8 && offset <= length; ++offset) {
      const Slice slice = Slice(data).substr(offset);
      EXPECT_EQ(crc32c::Extend(0, slice), crc32c::ExtendPortable(0, slice))
          << "length " << length << " offset " << offset;
    }
  }
}

TEST(Crc32c, MaskRoundTrips) {
  for (const std::uint32_t crc : {0U, 1U, 0x8A9136AAU, 0xE3069283U, 0xFFFFFFFFU}) {
    EXPECT_EQ(crc32c::Unmask(crc32c::Mask(crc)), crc);
  }
}

TEST(Crc32c, MaskChangesTheValue) {
  // If Mask were the identity, computing a checksum over a buffer that already
  // contains one could cancel out, which is the coincidence it exists to break.
  EXPECT_NE(crc32c::Mask(0x8A9136AAU), 0x8A9136AAU);
  EXPECT_NE(crc32c::Mask(0U), 0U);
}

TEST(Crc32c, MaskIsConstexpr) {
  static_assert(crc32c::Unmask(crc32c::Mask(0xDEADBEEFU)) == 0xDEADBEEFU);
  SUCCEED();
}

}  // namespace

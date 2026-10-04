#include "table/bloom_filter.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using strata::table::BloomFilter;

TEST(BloomFilterTest, EncodingIsStableAcrossReleases) {
  EXPECT_EQ(BloomFilter::Hash(""), 0xF52A15E9A9B5E89BULL);
  EXPECT_EQ(BloomFilter::Hash("abc"), 0x0DD490490804B508ULL);
  EXPECT_EQ(BloomFilter::Hash(std::string("\0\xff", 2)), 0xA70BEE94C6C8447BULL);
  const std::uint64_t hash = BloomFilter::Hash("abc");
  const std::string encoded = BloomFilter::Build({&hash, 1});
  EXPECT_EQ(encoded, std::string("\x07\x00\x01\x02\x04\x08\x10\x20\x40", 9));
}

TEST(BloomFilterTest, EveryInsertedKeyMayMatch) {
  std::vector<std::uint64_t> hashes;
  std::vector<std::string> keys = {"", "a", std::string("\0\xff", 2)};
  for (int index = 0; index < 1000; ++index) {
    keys.push_back("key-" + std::to_string(index));
  }
  for (const auto& key : keys) {
    hashes.push_back(BloomFilter::Hash(key));
  }
  const std::string filter = BloomFilter::Build(hashes);
  ASSERT_TRUE(BloomFilter::Validate(filter).ok());
  for (const auto& key : keys) {
    EXPECT_TRUE(BloomFilter::MayContain(key, filter)) << key;
  }
}

TEST(BloomFilterTest, MostMissingKeysAreRejected) {
  std::vector<std::uint64_t> hashes;
  for (int index = 0; index < 1000; ++index) {
    hashes.push_back(BloomFilter::Hash("present-" + std::to_string(index)));
  }
  const std::string filter = BloomFilter::Build(hashes);
  int false_positives = 0;
  for (int index = 0; index < 1000; ++index) {
    false_positives += BloomFilter::MayContain("absent-" + std::to_string(index), filter) ? 1 : 0;
  }
  EXPECT_LT(false_positives, 50);
}

TEST(BloomFilterTest, EmptyAndMalformedFilters) {
  const std::string empty = BloomFilter::Build({});
  ASSERT_TRUE(BloomFilter::Validate(empty).ok());
  EXPECT_FALSE(BloomFilter::MayContain("anything", empty));
  EXPECT_TRUE(BloomFilter::Validate("").IsCorruption());
  EXPECT_TRUE(BloomFilter::Validate(std::string(8, '\0')).IsCorruption());
  std::string wrong_probes = empty;
  wrong_probes[0] = '\x06';
  EXPECT_TRUE(BloomFilter::Validate(wrong_probes).IsCorruption());
  EXPECT_TRUE(BloomFilter::Validate(empty + "x").IsCorruption());
}

}  // namespace

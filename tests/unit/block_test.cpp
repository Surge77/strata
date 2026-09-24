#include "table/block.hpp"

#include <map>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "table/block_builder.hpp"
#include "util/coding.hpp"
#include "util/comparator.hpp"

namespace {

using strata::BytewiseComparator;
using strata::Slice;
using strata::table::Block;
using strata::table::BlockBuilder;

std::map<std::string, std::string> SampleEntries(std::size_t count, std::uint32_t seed) {
  std::mt19937 random(seed);
  std::map<std::string, std::string> entries;
  while (entries.size() < count) {
    // Shared prefixes, as real sorted keys have, so prefix compression is hit.
    const std::string key = "user:" + std::to_string(random() % 100000) + ":" +
                            std::string(random() % 5, static_cast<char>('a' + random() % 26));
    entries[key] = std::string(random() % 40, static_cast<char>('A' + random() % 26));
  }
  return entries;
}

std::string BuildBlock(const std::map<std::string, std::string>& entries, int restart_interval) {
  BlockBuilder builder(BytewiseComparator(), restart_interval);
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }
  return std::string(builder.Finish());
}

std::unique_ptr<Block> Parse(std::string contents) {
  auto block = Block::Create(std::move(contents));
  EXPECT_TRUE(block.ok()) << block.status().ToString();
  return block.ok() ? std::move(block).value() : nullptr;
}

TEST(Block, EmptyBlockHasNoEntries) {
  BlockBuilder builder(BytewiseComparator(), 16);
  EXPECT_TRUE(builder.empty());
  const auto block = Parse(std::string(builder.Finish()));
  auto it = block->NewIterator(BytewiseComparator());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->SeekToLast();
  EXPECT_FALSE(it->Valid());
  it->Seek("anything");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

class BlockRoundTrip : public ::testing::TestWithParam<int> {};

// Every restart interval from "every entry is a restart" to "one restart for
// the whole block" must round-trip identically.
TEST_P(BlockRoundTrip, IteratesSeeksAndReversesExactly) {
  const auto entries = SampleEntries(500, 7);
  const auto block = Parse(BuildBlock(entries, GetParam()));
  auto it = block->NewIterator(BytewiseComparator());

  auto expected = entries.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++expected) {
    ASSERT_NE(expected, entries.end());
    ASSERT_EQ(it->key(), expected->first);
    ASSERT_EQ(it->value(), expected->second);
  }
  EXPECT_EQ(expected, entries.end());

  auto reverse = entries.rbegin();
  for (it->SeekToLast(); it->Valid(); it->Prev(), ++reverse) {
    ASSERT_NE(reverse, entries.rend());
    ASSERT_EQ(it->key(), reverse->first);
  }
  EXPECT_EQ(reverse, entries.rend());

  // Seek to every key, and to keys just before and after it.
  for (const auto& [key, value] : entries) {
    it->Seek(key);
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(it->key(), key);
    ASSERT_EQ(it->value(), value);
    const std::string before = key.substr(0, key.size() - 1);
    it->Seek(before);
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(it->key(), entries.lower_bound(before)->first);
  }
  it->Seek("\xff");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

INSTANTIATE_TEST_SUITE_P(RestartIntervals, BlockRoundTrip, ::testing::Values(1, 2, 16, 1000));

TEST(Block, PrefixCompressionShrinksSortedKeys) {
  std::map<std::string, std::string> entries;
  for (int index = 0; index < 200; ++index) {
    entries["a-very-long-common-key-prefix/" + std::to_string(10000 + index)] = "v";
  }
  const std::size_t compressed = BuildBlock(entries, 16).size();
  const std::size_t uncompressed = BuildBlock(entries, 1).size();
  EXPECT_LT(compressed * 2, uncompressed) << compressed << " vs " << uncompressed;
}

TEST(Block, RejectsImpossibleRestartArrays) {
  EXPECT_TRUE(Block::Create("").status().IsCorruption());
  EXPECT_TRUE(Block::Create("abc").status().IsCorruption());
  std::string zero_restarts;
  strata::PutFixed32(&zero_restarts, 0);
  EXPECT_TRUE(Block::Create(zero_restarts).status().IsCorruption());
  std::string too_many;
  strata::PutFixed32(&too_many, 1000);
  EXPECT_TRUE(Block::Create(too_many).status().IsCorruption());
  std::string restart_past_entries;
  strata::PutFixed32(&restart_past_entries, 99);  // restart[0] = 99, but no entries
  strata::PutFixed32(&restart_past_entries, 1);
  EXPECT_TRUE(Block::Create(restart_past_entries).status().IsCorruption());
}

// Flip every byte of a real block, one at a time, three ways. The block must
// either be rejected by Create or iterate to completion in both directions;
// under ASan any out-of-bounds read fails the test. (A flipped value byte
// yields a legal block with a different value -- detecting that is the job of
// the block checksum, not the parser, so only memory safety and termination
// are asserted here.)
TEST(Block, EveryCorruptedByteIsHandledSafely) {
  const auto entries = SampleEntries(60, 3);
  const std::string pristine = BuildBlock(entries, 4);
  for (std::size_t offset = 0; offset < pristine.size(); ++offset) {
    for (const unsigned flip : {0x01U, 0x80U, 0xFFU}) {
      std::string damaged = pristine;
      damaged[offset] = static_cast<char>(static_cast<unsigned char>(damaged[offset]) ^ flip);
      auto block = Block::Create(damaged);
      if (!block.ok()) {
        continue;
      }
      auto it = (*block)->NewIterator(BytewiseComparator());
      std::size_t steps = 0;
      for (it->SeekToFirst(); it->Valid() && steps < 10000; it->Next()) {
        ++steps;
      }
      for (it->SeekToLast(); it->Valid() && steps < 20000; it->Prev()) {
        ++steps;
      }
      it->Seek("user:5");
      ASSERT_LT(steps, 20000U) << "iteration did not terminate at offset " << offset;
    }
  }
}

}  // namespace

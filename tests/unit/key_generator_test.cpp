#include "stratabench/key_generator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using strata::bench::KeyChooser;
using strata::bench::KeyDistribution;
using strata::bench::ZipfianGenerator;

constexpr std::uint64_t kItems = 1000;
constexpr int kDraws = 200'000;

std::vector<int> Histogram(KeyChooser* chooser) {
  std::vector<int> counts(kItems, 0);
  for (int draw = 0; draw < kDraws; ++draw) {
    const std::uint64_t key = chooser->Next();
    EXPECT_LT(key, kItems);
    if (key < kItems) {
      ++counts[key];
    }
  }
  return counts;
}

TEST(KeyGenerator, UniformStaysInRangeAndCoversIt) {
  KeyChooser chooser(KeyDistribution::kUniform, kItems, /*seed=*/1);
  const std::vector<int> counts = Histogram(&chooser);
  // 200 expected per key; a key drawn fewer than 100 times would be a 7-sigma event.
  EXPECT_GT(*std::min_element(counts.begin(), counts.end()), 100);
  EXPECT_LT(*std::max_element(counts.begin(), counts.end()), 300);
}

TEST(KeyGenerator, SameSeedGivesTheSameSequence) {
  for (const auto distribution : {KeyDistribution::kUniform, KeyDistribution::kZipfian}) {
    KeyChooser first(distribution, kItems, /*seed=*/7);
    KeyChooser second(distribution, kItems, /*seed=*/7);
    for (int draw = 0; draw < 1000; ++draw) {
      ASSERT_EQ(first.Next(), second.Next());
    }
  }
}

// Rank 0 has probability 1 / zeta(n, theta), the defining property of Zipf.
TEST(KeyGenerator, ZipfianRankZeroHasItsTheoreticalProbability) {
  double zeta = 0;
  for (std::uint64_t rank = 1; rank <= kItems; ++rank) {
    zeta += 1.0 / std::pow(static_cast<double>(rank), ZipfianGenerator::kDefaultTheta);
  }
  ZipfianGenerator zipf(kItems, ZipfianGenerator::kDefaultTheta, /*seed=*/3);
  int zeros = 0;
  for (int draw = 0; draw < kDraws; ++draw) {
    const std::uint64_t rank = zipf.NextRank();
    ASSERT_LT(rank, kItems);
    zeros += rank == 0 ? 1 : 0;
  }
  EXPECT_NEAR(static_cast<double>(zeros) / kDraws, 1.0 / zeta, 0.01);
}

TEST(KeyGenerator, ZipfianPopularityFallsWithRank) {
  ZipfianGenerator zipf(kItems, ZipfianGenerator::kDefaultTheta, /*seed=*/4);
  std::vector<int> counts(kItems, 0);
  for (int draw = 0; draw < kDraws; ++draw) {
    ++counts[zipf.NextRank()];
  }
  EXPECT_GT(counts[0], counts[1]);
  EXPECT_GT(counts[1], counts[10]);
  EXPECT_GT(counts[10], counts[100]);
  EXPECT_GT(counts[100], counts[900]);
}

// Scrambling must be a permutation: every rank lands on its own key, so the
// hot set keeps exactly the size Zipf gives it.
TEST(KeyGenerator, ScramblingIsAPermutationOfTheKeySpace) {
  std::set<std::uint64_t> keys;
  for (std::uint64_t rank = 0; rank < kItems; ++rank) {
    keys.insert(ZipfianGenerator::Scramble(rank, kItems));
  }
  EXPECT_EQ(keys.size(), kItems);
  EXPECT_LT(*keys.rbegin(), kItems);
}

// Without scrambling the ten hottest keys would be 0..9, all in the first table.
TEST(KeyGenerator, ScrambledZipfianSpreadsTheHotKeysAcrossTheKeySpace) {
  KeyChooser chooser(KeyDistribution::kZipfian, kItems, /*seed=*/5);
  const std::vector<int> counts = Histogram(&chooser);
  std::vector<std::pair<int, std::uint64_t>> by_count;
  for (std::uint64_t key = 0; key < kItems; ++key) {
    by_count.emplace_back(counts[key], key);
  }
  std::sort(by_count.rbegin(), by_count.rend());
  std::uint64_t low = kItems;
  std::uint64_t high = 0;
  for (std::size_t index = 0; index < 10; ++index) {
    low = std::min(low, by_count[index].second);
    high = std::max(high, by_count[index].second);
  }
  EXPECT_GT(high - low, kItems / 2);
  // And it is still skewed: the hottest key alone takes several percent of draws.
  EXPECT_GT(by_count[0].first, kDraws / 50);
}

TEST(KeyGenerator, SingleItemAlwaysChoosesKeyZero) {
  for (const auto distribution : {KeyDistribution::kUniform, KeyDistribution::kZipfian}) {
    KeyChooser chooser(distribution, 1, /*seed=*/9);
    for (int draw = 0; draw < 100; ++draw) {
      ASSERT_EQ(chooser.Next(), 0U);
    }
  }
}

}  // namespace

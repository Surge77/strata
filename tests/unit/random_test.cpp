#include "util/random.hpp"

#include <cstdint>
#include <set>

#include <gtest/gtest.h>

namespace {

using strata::Random;

TEST(Random, SameSeedGivesTheSameSequence) {
  Random a(123);
  Random b(123);
  for (int index = 0; index < 1000; ++index) {
    ASSERT_EQ(a.Next(), b.Next());
  }
}

TEST(Random, DifferentSeedsDiverge) {
  Random a(1);
  Random b(2);
  int equal = 0;
  for (int index = 0; index < 100; ++index) {
    equal += a.Next() == b.Next() ? 1 : 0;
  }
  EXPECT_EQ(equal, 0);
}

TEST(Random, UniformStaysInRangeAndCoversIt) {
  Random random(7);
  std::set<std::uint64_t> seen;
  for (int index = 0; index < 10000; ++index) {
    const std::uint64_t value = random.Uniform(10);
    ASSERT_LT(value, 10U);
    seen.insert(value);
  }
  EXPECT_EQ(seen.size(), 10U);
}

// Skiplist balance depends on OneIn(4) really being about 1 in 4.
TEST(Random, OneInHasTheRightFrequency) {
  Random random(99);
  constexpr int kTrials = 100000;
  int hits = 0;
  for (int index = 0; index < kTrials; ++index) {
    hits += random.OneIn(4) ? 1 : 0;
  }
  EXPECT_NEAR(static_cast<double>(hits) / kTrials, 0.25, 0.01);
}

}  // namespace

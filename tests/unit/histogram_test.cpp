#include "util/histogram.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace {

using strata::Histogram;

// Exact percentile of a sorted sample set, using the same "value at rank
// ceil(p% * n)" definition the histogram approximates.
double ExactPercentile(const std::vector<std::uint64_t>& sorted, double percent) {
  const auto rank =
      static_cast<std::size_t>(std::ceil(percent / 100.0 * static_cast<double>(sorted.size())));
  return static_cast<double>(sorted[std::max<std::size_t>(rank, 1) - 1]);
}

TEST(Histogram, EmptyHistogramReportsZeros) {
  const Histogram histogram;
  EXPECT_EQ(histogram.Count(), 0U);
  EXPECT_EQ(histogram.Min(), 0U);
  EXPECT_EQ(histogram.Max(), 0U);
  EXPECT_DOUBLE_EQ(histogram.Mean(), 0.0);
  EXPECT_DOUBLE_EQ(histogram.StandardDeviation(), 0.0);
  EXPECT_DOUBLE_EQ(histogram.Percentile(50), 0.0);
}

TEST(Histogram, TracksExactSummaryStatistics) {
  Histogram histogram;
  for (const std::uint64_t value : {2U, 4U, 4U, 4U, 5U, 5U, 7U, 9U}) {
    histogram.Add(value);
  }
  EXPECT_EQ(histogram.Count(), 8U);
  EXPECT_EQ(histogram.Min(), 2U);
  EXPECT_EQ(histogram.Max(), 9U);
  EXPECT_DOUBLE_EQ(histogram.Mean(), 5.0);
  EXPECT_DOUBLE_EQ(histogram.StandardDeviation(), 2.0);  // textbook population example
}

// A steady floor with tiny jitter: the naive E[x^2] - E[x]^2 formula returns
// garbage here because 1e12^2 swamps a variance of 2/3. Welford does not.
TEST(Histogram, DeviationStaysAccurateNearALargeMean) {
  Histogram histogram;
  constexpr std::uint64_t kFloor = 1'000'000'000'000ULL;
  for (int round = 0; round < 1000; ++round) {
    histogram.Add(kFloor);
    histogram.Add(kFloor + 1);
    histogram.Add(kFloor + 2);
  }
  EXPECT_NEAR(histogram.StandardDeviation(), std::sqrt(2.0 / 3.0), 1e-3);
  EXPECT_NEAR(histogram.Mean(), static_cast<double>(kFloor) + 1.0, 1e-3);
}

TEST(Histogram, MergedDeviationMatchesTheCombinedSamples) {
  Histogram left;
  Histogram right;
  Histogram both;
  for (std::uint64_t value = 1; value <= 1000; ++value) {
    left.Add(value);
    both.Add(value);
    right.Add(value * 1000);
    both.Add(value * 1000);
  }
  left.Merge(right);
  EXPECT_NEAR(left.StandardDeviation(), both.StandardDeviation(), 1e-6 * both.StandardDeviation());
  EXPECT_NEAR(left.Mean(), both.Mean(), 1e-9 * both.Mean());
}

TEST(Histogram, SmallValuesHaveExactBuckets) {
  for (std::uint64_t value = 0; value < 16; ++value) {
    EXPECT_EQ(Histogram::BucketFor(value), value);
    EXPECT_EQ(Histogram::BucketLowerBound(value), value);
    EXPECT_EQ(Histogram::BucketUpperBound(value), value + 1);
  }
}

// Every value must land in a bucket whose range contains it, and every bucket
// must be no wider than 1/16 of its lower bound -- the accuracy guarantee.
TEST(Histogram, EveryValueLiesInItsBucketsRange) {
  std::mt19937_64 random(42);
  std::vector<std::uint64_t> values = {
      16, 17, 31, 32, 33, 1000, 1U << 20, UINT64_MAX, UINT64_MAX - 1, 1ULL << 63};
  for (int index = 0; index < 100000; ++index) {
    values.push_back(random() >> (random() % 64));
  }
  for (const std::uint64_t value : values) {
    const std::size_t bucket = Histogram::BucketFor(value);
    ASSERT_LT(bucket, Histogram::kBucketCount);
    const std::uint64_t lower = Histogram::BucketLowerBound(bucket);
    const std::uint64_t upper = Histogram::BucketUpperBound(bucket);
    ASSERT_LE(lower, value);
    ASSERT_TRUE(value < upper || (upper == UINT64_MAX && value == UINT64_MAX)) << value;
    if (lower >= 16) {
      ASSERT_LE(upper - lower, lower / 16) << "bucket " << bucket;
    }
  }
}

TEST(Histogram, BucketsAreContiguous) {
  for (std::size_t bucket = 0; bucket + 1 < Histogram::kBucketCount; ++bucket) {
    ASSERT_EQ(Histogram::BucketUpperBound(bucket), Histogram::BucketLowerBound(bucket + 1));
  }
  EXPECT_EQ(Histogram::BucketFor(UINT64_MAX), Histogram::kBucketCount - 1);
}

TEST(Histogram, PercentilesAreWithinTheStatedErrorBound) {
  std::mt19937_64 random(7);
  std::lognormal_distribution<double> latency(6.0, 1.5);  // a long-tailed shape
  Histogram histogram;
  std::vector<std::uint64_t> samples;
  for (int index = 0; index < 200000; ++index) {
    const auto value = static_cast<std::uint64_t>(latency(random));
    samples.push_back(value);
    histogram.Add(value);
  }
  std::sort(samples.begin(), samples.end());
  for (const double percent : {1.0, 10.0, 50.0, 90.0, 99.0, 99.9}) {
    const double exact = ExactPercentile(samples, percent);
    const double estimate = histogram.Percentile(percent);
    EXPECT_NEAR(estimate, exact, std::max(exact / 16.0, 1.0)) << "p" << percent;
  }
}

TEST(Histogram, ExtremePercentilesAreExact) {
  Histogram histogram;
  for (const std::uint64_t value : {100U, 250U, 1234U, 99999U}) {
    histogram.Add(value);
  }
  EXPECT_DOUBLE_EQ(histogram.Percentile(0), 100.0);
  EXPECT_DOUBLE_EQ(histogram.Percentile(100), 99999.0);
  EXPECT_LE(histogram.Percentile(99.99), 99999.0);
  EXPECT_GE(histogram.Percentile(0.01), 100.0);
}

TEST(Histogram, MergeEqualsAddingEverySample) {
  Histogram left;
  Histogram right;
  Histogram both;
  for (std::uint64_t value = 0; value < 5000; ++value) {
    (value % 3 == 0 ? left : right).Add(value * 17);
    both.Add(value * 17);
  }
  left.Merge(right);
  EXPECT_EQ(left.Count(), both.Count());
  EXPECT_EQ(left.Min(), both.Min());
  EXPECT_EQ(left.Max(), both.Max());
  EXPECT_NEAR(left.Mean(), both.Mean(), 1e-9 * both.Mean());
  EXPECT_DOUBLE_EQ(left.Percentile(99), both.Percentile(99));
}

TEST(Histogram, MergingAnEmptyHistogramChangesNothing) {
  Histogram histogram;
  histogram.Add(10);
  histogram.Merge(Histogram());
  EXPECT_EQ(histogram.Count(), 1U);
  EXPECT_EQ(histogram.Min(), 10U);
}

TEST(Histogram, ClearResetsEverything) {
  Histogram histogram;
  histogram.Add(5);
  histogram.Clear();
  EXPECT_EQ(histogram.Count(), 0U);
  EXPECT_EQ(histogram.Max(), 0U);
  histogram.Add(3);
  EXPECT_EQ(histogram.Min(), 3U);
}

TEST(Histogram, ConstantSamplesHaveZeroDeviation) {
  Histogram histogram;
  for (int index = 0; index < 1000; ++index) {
    histogram.Add(123456789);
  }
  EXPECT_DOUBLE_EQ(histogram.StandardDeviation(), 0.0);
  EXPECT_DOUBLE_EQ(histogram.Percentile(50), 123456789.0);
}

TEST(Histogram, ToStringNamesTheSummaryFields) {
  Histogram histogram;
  histogram.Add(1);
  histogram.Add(3);
  const std::string text = histogram.ToString();
  for (const char* field : {"count=2", "mean=2.0", "min=1", "p50=", "p99=", "max=3"}) {
    EXPECT_NE(text.find(field), std::string::npos) << field << " in " << text;
  }
}

}  // namespace

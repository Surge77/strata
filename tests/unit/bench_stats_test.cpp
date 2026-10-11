#include <vector>

#include <gtest/gtest.h>

#include "stratabench/stats.hpp"

namespace {

using strata::bench::Protocol;
using strata::bench::RunRounds;
using strata::bench::Summarize;
using strata::bench::Summary;

TEST(BenchStats, EmptyInputSummarizesToZeros) {
  const Summary summary = Summarize({});
  EXPECT_EQ(summary.rounds, 0U);
  EXPECT_DOUBLE_EQ(summary.median, 0.0);
  EXPECT_DOUBLE_EQ(summary.cv, 0.0);
}

TEST(BenchStats, MedianOfOddCountIsTheMiddleValueWhateverTheOrder) {
  const std::vector<double> values = {30, 10, 50, 20, 40};
  const Summary summary = Summarize(values);
  EXPECT_DOUBLE_EQ(summary.median, 30.0);
  EXPECT_DOUBLE_EQ(summary.min, 10.0);
  EXPECT_DOUBLE_EQ(summary.max, 50.0);
  EXPECT_DOUBLE_EQ(summary.mean, 30.0);
  EXPECT_EQ(summary.rounds, 5U);
}

TEST(BenchStats, MedianOfEvenCountAveragesTheMiddlePair) {
  const std::vector<double> values = {4, 1, 3, 2};
  EXPECT_DOUBLE_EQ(Summarize(values).median, 2.5);
}

// Sample (n - 1) deviation, not population: {9, 10, 11} has sample variance 1,
// so the CV is 1 / 10. The population formula would give 0.0816.
TEST(BenchStats, CoefficientOfVariationUsesTheSampleDeviation) {
  const std::vector<double> values = {9, 10, 11};
  EXPECT_DOUBLE_EQ(Summarize(values).cv, 0.1);
}

TEST(BenchStats, IdenticalRoundsHaveZeroVariation) {
  const std::vector<double> values = {7, 7, 7, 7, 7};
  const Summary summary = Summarize(values);
  EXPECT_DOUBLE_EQ(summary.cv, 0.0);
  EXPECT_TRUE(summary.stable());
}

TEST(BenchStats, SingleRoundHasNoMeasurableVariation) {
  const std::vector<double> values = {42};
  const Summary summary = Summarize(values);
  EXPECT_DOUBLE_EQ(summary.median, 42.0);
  EXPECT_DOUBLE_EQ(summary.cv, 0.0);
}

TEST(BenchStats, ZeroMeanDoesNotDivideByZero) {
  const std::vector<double> values = {0, 0, 0};
  EXPECT_DOUBLE_EQ(Summarize(values).cv, 0.0);
}

TEST(BenchStats, StableMeansCvAtMostFivePercent) {
  EXPECT_TRUE(Summary{.cv = 0.05}.stable());
  EXPECT_FALSE(Summary{.cv = 0.0501}.stable());
}

TEST(BenchStats, NoisyRoundsAreUnstable) {
  const std::vector<double> values = {100, 120, 80, 110, 90};
  EXPECT_FALSE(Summarize(values).stable());
}

TEST(BenchStats, FewerThanFiveRoundsIsNotPublishable) {
  EXPECT_FALSE((Protocol{.warmup = 1, .rounds = 4}).publishable());
  EXPECT_TRUE((Protocol{.warmup = 0, .rounds = 5}).publishable());
}

TEST(BenchStats, RunRoundsDiscardsWarmupRounds) {
  int calls = 0;
  const std::vector<double> values =
      RunRounds(Protocol{.warmup = 2, .rounds = 3}, [&] { return static_cast<double>(calls++); });
  EXPECT_EQ(calls, 5);
  EXPECT_EQ(values, (std::vector<double>{2, 3, 4}));
}

TEST(BenchStats, RunRoundsTreatsNegativeCountsAsZero) {
  int calls = 0;
  const std::vector<double> values = RunRounds(Protocol{.warmup = -1, .rounds = -1}, [&] {
    ++calls;
    return 1.0;
  });
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(values.empty());
}

}  // namespace

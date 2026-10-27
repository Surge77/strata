#include "util/metrics.hpp"

#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "util/histogram.hpp"

namespace strata {
namespace {

TEST(CounterTest, StartsAtZeroAndAdds) {
  Counter counter;
  EXPECT_EQ(counter.Value(), 0U);
  counter.Add();
  counter.Add(41);
  EXPECT_EQ(counter.Value(), 42U);
}

TEST(CounterTest, OccupiesItsOwnCacheLine) {
  EXPECT_EQ(alignof(Counter), kCacheLineSize);
  EXPECT_EQ(sizeof(Counter), kCacheLineSize);
}

TEST(ConcurrentHistogramTest, EmptySnapshotIsEmpty) {
  const ConcurrentHistogram histogram;
  const Histogram snapshot = histogram.Snapshot();
  EXPECT_EQ(snapshot.Count(), 0U);
  EXPECT_EQ(snapshot.Min(), 0U);
  EXPECT_EQ(snapshot.Max(), 0U);
  EXPECT_EQ(snapshot.Percentile(99), 0.0);
}

TEST(ConcurrentHistogramTest, MatchesAPlainHistogramOfTheSameSamples) {
  ConcurrentHistogram concurrent;
  Histogram plain;
  for (std::uint64_t value = 1; value <= 5000; value += 7) {
    concurrent.Add(value * value % 9973);
    plain.Add(value * value % 9973);
  }
  const Histogram snapshot = concurrent.Snapshot();
  EXPECT_EQ(snapshot.Count(), plain.Count());
  EXPECT_EQ(snapshot.Min(), plain.Min());
  EXPECT_EQ(snapshot.Max(), plain.Max());
  EXPECT_NEAR(snapshot.Mean(), plain.Mean(), plain.Mean() * 1e-12);
  for (const double percent : {1.0, 50.0, 90.0, 99.0, 99.9}) {
    EXPECT_DOUBLE_EQ(snapshot.Percentile(percent), plain.Percentile(percent)) << percent;
  }
}

TEST(ConcurrentHistogramTest, CountsEverySampleFromManyThreads) {
  constexpr int kThreads = 12;  // more than kShards, so shards are shared
  constexpr std::uint64_t kPerThread = 20000;
  ConcurrentHistogram histogram;
  std::vector<std::thread> threads;
  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&histogram, thread] {
      for (std::uint64_t i = 0; i < kPerThread; ++i) {
        histogram.Add(static_cast<std::uint64_t>(thread) * 1000 + i % 1000);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const Histogram snapshot = histogram.Snapshot();
  EXPECT_EQ(snapshot.Count(), kThreads * kPerThread);
  EXPECT_EQ(snapshot.Min(), 0U);
  EXPECT_EQ(snapshot.Max(), (kThreads - 1) * 1000U + 999U);
}

TEST(MetricsRegistryTest, ReportsEveryMetricByName) {
  MetricsRegistry registry;
  Counter& gets = registry.AddCounter("get.calls");
  Counter& puts = registry.AddCounter("write.calls");
  ConcurrentHistogram& latency = registry.AddLatency("get");
  gets.Add(3);
  puts.Add();
  latency.Add(10);
  latency.Add(20);

  const auto counters = registry.CounterValues();
  ASSERT_EQ(counters.size(), 2U);
  EXPECT_EQ(counters.at("get.calls"), 3U);
  EXPECT_EQ(counters.at("write.calls"), 1U);
  const auto latencies = registry.LatencyValues();
  ASSERT_EQ(latencies.size(), 1U);
  EXPECT_EQ(latencies.at("get").count, 2U);
  EXPECT_DOUBLE_EQ(latencies.at("get").mean, 15.0);
  EXPECT_EQ(latencies.at("get").max, 20U);
}

TEST(MetricsRegistryTest, ReferencesSurviveLaterRegistrations) {
  MetricsRegistry registry;
  Counter& first = registry.AddCounter("first");
  for (int i = 0; i < 1000; ++i) {
    registry.AddCounter("filler." + std::to_string(i));
  }
  first.Add(7);
  EXPECT_EQ(registry.CounterValues().at("first"), 7U);
}

TEST(SummarizeTest, CopiesThePercentiles) {
  Histogram histogram;
  for (std::uint64_t value = 1; value <= 1000; ++value) {
    histogram.Add(value);
  }
  const LatencyStats stats = Summarize(histogram);
  EXPECT_EQ(stats.count, 1000U);
  EXPECT_DOUBLE_EQ(stats.p50, histogram.Percentile(50));
  EXPECT_DOUBLE_EQ(stats.p999, histogram.Percentile(99.9));
  EXPECT_EQ(stats.max, 1000U);
}

}  // namespace
}  // namespace strata

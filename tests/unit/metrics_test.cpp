#include "util/metrics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <set>
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

TEST(CounterTest, GivesEachShardItsOwnCacheLine) {
  EXPECT_EQ(alignof(Counter), kCacheLineSize);
  EXPECT_EQ(sizeof(Counter), (kMetricShards + 1) * kCacheLineSize);
}

TEST(CounterTest, NeverGivesTwoLiveThreadsTheSameExclusiveSlot) {
  constexpr std::size_t kThreads = kMetricShards + 2;
  std::array<std::size_t, kThreads> slots{};
  std::latch all_claimed(kThreads);
  std::vector<std::thread> threads;
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&slots, &all_claimed, thread] {
      slots[thread] = ThisThreadCounterSlot();
      all_claimed.arrive_and_wait();  // stay alive until every thread holds its slot
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  std::set<std::size_t> exclusive;
  std::size_t shared = 0;
  for (const std::size_t slot : slots) {
    if (slot == kSharedCounterSlot) {
      ++shared;
    } else {
      EXPECT_LT(slot, kMetricShards);
      EXPECT_TRUE(exclusive.insert(slot).second) << "slot " << slot << " was handed out twice";
    }
  }
  EXPECT_GE(shared, 2U);
}

TEST(CounterTest, FreesASlotWhenItsThreadExits) {
  // One short of every slot, because this thread may already hold one. Without
  // the release, the second round would find them taken.
  constexpr std::size_t kThreads = kMetricShards - 1;
  for (int round = 0; round < 3; ++round) {
    std::array<std::size_t, kThreads> slots{};
    std::latch all_claimed(kThreads);
    std::vector<std::thread> threads;
    for (std::size_t thread = 0; thread < kThreads; ++thread) {
      threads.emplace_back([&slots, &all_claimed, thread] {
        slots[thread] = ThisThreadCounterSlot();
        all_claimed.arrive_and_wait();
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
    for (const std::size_t slot : slots) {
      EXPECT_LT(slot, kMetricShards) << "round " << round;
    }
  }
}

TEST(CounterTest, KeepsEveryAddWhenSlotsPassBetweenThreads) {
  constexpr int kRounds = 50;
  constexpr int kThreads = 12;  // more than kMetricShards, so some share a slot
  constexpr std::uint64_t kPerThread = 1000;
  Counter counter;
  for (int round = 0; round < kRounds; ++round) {
    std::vector<std::thread> threads;
    for (int thread = 0; thread < kThreads; ++thread) {
      threads.emplace_back([&counter] {
        for (std::uint64_t i = 0; i < kPerThread; ++i) {
          counter.Add();
        }
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
  }
  EXPECT_EQ(counter.Value(), kRounds * kThreads * kPerThread);
}

TEST(CounterTest, CountsEveryAddFromManyThreads) {
  constexpr int kThreads = 12;  // more than kMetricShards, so shards are shared
  constexpr std::uint64_t kPerThread = 20000;
  Counter counter;
  std::vector<std::thread> threads;
  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&counter] {
      for (std::uint64_t i = 0; i < kPerThread; ++i) {
        counter.Add();
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(counter.Value(), kThreads * kPerThread);
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
}

TEST(MetricsRegistryTest, RecordsNanosecondsAndReportsMicroseconds) {
  MetricsRegistry registry;
  ConcurrentHistogram& latency = registry.AddLatency("get");
  // Sub-microsecond samples must not round to zero: a memtable Get is one.
  latency.Add(300);
  latency.Add(2700);
  const LatencyStats stats = registry.LatencyValues().at("get");
  EXPECT_EQ(stats.count, 2U);
  EXPECT_DOUBLE_EQ(stats.mean, 1.5);
  EXPECT_DOUBLE_EQ(stats.max, 2.7);
  EXPECT_GT(stats.p50, 0.0);
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
  EXPECT_DOUBLE_EQ(stats.max, 1000.0);
}

TEST(SummarizeTest, DividesEveryFigureByTheUnit) {
  Histogram histogram;
  histogram.Add(1000);
  histogram.Add(3000);
  const LatencyStats stats = Summarize(histogram, 1000.0);
  EXPECT_DOUBLE_EQ(stats.mean, 2.0);
  EXPECT_DOUBLE_EQ(stats.max, 3.0);
  EXPECT_DOUBLE_EQ(stats.p50, histogram.Percentile(50) / 1000.0);
}

// A snapshot taken while a shard's very first sample is in flight can see
// the bucket before min and max are set. It must still report figures
// inside the bucket, not min = UINT64_MAX and max = 0.
TEST(HistogramFromBucketsTest, RepairsMinAndMaxTornByAConcurrentFirstSample) {
  std::array<std::uint64_t, Histogram::kBucketCount> buckets{};
  buckets[Histogram::BucketFor(500)] = 1;
  const Histogram torn = Histogram::FromBuckets(buckets, UINT64_MAX, 0, 500);
  EXPECT_LE(torn.Min(), 500U);
  EXPECT_GE(torn.Max(), 500U);
  EXPECT_LE(torn.Min(), torn.Max());
  const double p50 = torn.Percentile(50);
  EXPECT_GE(p50, static_cast<double>(torn.Min()));
  EXPECT_LE(p50, static_cast<double>(torn.Max()));
}

}  // namespace
}  // namespace strata

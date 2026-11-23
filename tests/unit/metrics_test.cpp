#include "util/metrics.hpp"

#include <array>
#include <atomic>
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
  EXPECT_EQ(sizeof(Counter), 2 * kMetricShards * kCacheLineSize);
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

TEST(CounterTest, KeepsEveryAddWhileSlotsPassBetweenThreadsUnderAReader) {
  constexpr int kRounds = 20;
  // Twice the exclusive slots, so half the threads add through shared ones.
  constexpr std::size_t kThreads = 2 * kMetricShards;
  constexpr std::uint64_t kPerThread = 20'000;
  Counter counter;
  std::atomic<bool> done{false};
  std::atomic<bool> went_backwards{false};
  std::thread reader([&] {
    std::uint64_t last = 0;
    while (!done.load(std::memory_order_relaxed)) {
      const std::uint64_t now = counter.Value();
      if (now < last) {
        went_backwards = true;
      }
      last = now;
    }
  });
  for (int round = 0; round < kRounds; ++round) {
    // Released together, so every round's threads are alive at once and the
    // slots freed by the previous round are contended.
    std::latch start(kThreads);
    std::vector<std::thread> threads;
    for (std::size_t thread = 0; thread < kThreads; ++thread) {
      threads.emplace_back([&counter, &start] {
        start.arrive_and_wait();
        for (std::uint64_t i = 0; i < kPerThread; ++i) {
          counter.Add();
        }
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
  }
  done = true;
  reader.join();
  EXPECT_FALSE(went_backwards) << "a reader saw the total decrease";
  EXPECT_EQ(counter.Value(), kRounds * kThreads * kPerThread);
}

// Destroyed after the slot's release object, because it was constructed
// before the thread's first Add (thread_locals die in reverse order).
struct LateAdder {
  Counter* counter = nullptr;
  std::atomic<std::size_t>* slot_seen = nullptr;
  LateAdder() = default;
  LateAdder(const LateAdder&) = delete;
  LateAdder& operator=(const LateAdder&) = delete;
  LateAdder(LateAdder&&) = delete;
  LateAdder& operator=(LateAdder&&) = delete;
  ~LateAdder() {
    if (counter != nullptr) {
      slot_seen->store(ThisThreadCounterSlot());
      counter->Add(5);
    }
  }
};

TEST(CounterTest, AnAddAfterTheSlotIsReleasedGoesToASharedSlotAndCounts) {
  Counter counter;
  std::atomic<std::size_t> slot_seen{0};
  std::atomic<std::size_t> slot_held{kSharedCounterSlot};
  std::thread thread([&] {
    thread_local LateAdder late;
    late.counter = &counter;
    late.slot_seen = &slot_seen;
    counter.Add();
    slot_held = ThisThreadCounterSlot();
  });
  thread.join();
  ASSERT_LT(slot_held.load(), kMetricShards) << "the thread never held an exclusive slot";
  // Once released, the slot may already belong to another thread, so writing
  // it without an atomic read-modify-write could lose that thread's adds.
  EXPECT_EQ(slot_seen.load(), kSharedCounterSlot);
  EXPECT_EQ(counter.Value(), 6U);
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

TEST(SummarizeTest, ListsTheNonEmptyBucketsInOrder) {
  Histogram histogram;
  histogram.Add(3);
  histogram.Add(3);
  histogram.Add(1000);
  const LatencyStats stats = Summarize(histogram);
  ASSERT_EQ(stats.buckets.size(), 2U);
  EXPECT_EQ(stats.buckets[0].count, 2U);
  EXPECT_EQ(stats.buckets[1].count, 1U);
  // Exclusive upper bounds: each sample lies below its bucket's bound.
  EXPECT_GT(stats.buckets[0].upper_micros, 3.0);
  EXPECT_GT(stats.buckets[1].upper_micros, 1000.0);
  EXPECT_LE(stats.buckets[1].upper_micros, 1000.0 * 17 / 16);
  EXPECT_TRUE(Summarize(Histogram{}).buckets.empty());
}

TEST(SummarizeTest, BucketCountsAddUpToTheCount) {
  Histogram histogram;
  for (std::uint64_t value = 1; value <= 5000; value += 7) {
    histogram.Add(value);
  }
  const LatencyStats stats = Summarize(histogram, 1000.0);
  std::uint64_t total = 0;
  double previous_bound = 0.0;
  for (const LatencyBucket& bucket : stats.buckets) {
    EXPECT_GT(bucket.upper_micros, previous_bound);
    previous_bound = bucket.upper_micros;
    total += bucket.count;
  }
  EXPECT_EQ(total, stats.count);
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

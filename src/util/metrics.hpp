#pragma once

#include "strata/stats.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>

#include "util/histogram.hpp"

namespace strata {

// The size of a cache line on the x86-64 and ARM64 hosts this engine targets.
// Spelled out because std::hardware_destructive_interference_size makes GCC
// warn that its value may differ between translation units.
inline constexpr std::size_t kCacheLineSize = 64;

// Metrics bumped on every Get are split into this many shards, one per
// thread (round-robin), so concurrent readers do not pass one cache line
// back and forth. More threads than shards share them, which stays correct.
inline constexpr std::size_t kMetricShards = 8;

// The calling thread's shard, fixed the first time it asks.
[[nodiscard]] inline std::size_t ThisThreadShard() noexcept {
  static std::atomic<std::size_t> next{0};
  thread_local const std::size_t shard =
      next.fetch_add(1, std::memory_order_relaxed) % kMetricShards;
  return shard;
}

// A monotonic count, safe to bump from any thread without a lock. Relaxed
// ordering: a counter orders nothing, and readers only need each value to be
// one that was really reached. Each shard has a cache line of its own, so
// neither two threads nor two counters slow each other down.
class alignas(kCacheLineSize) Counter {
 public:
  void Add(std::uint64_t amount = 1) noexcept {
    shards_[ThisThreadShard()].value.fetch_add(amount, std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t Value() const noexcept {
    std::uint64_t total = 0;
    for (const Shard& shard : shards_) {
      total += shard.value.load(std::memory_order_relaxed);
    }
    return total;
  }

 private:
  struct alignas(kCacheLineSize) Shard {
    std::atomic<std::uint64_t> value{0};
  };
  std::array<Shard, kMetricShards> shards_{};
};

// A Histogram that many threads can add to at once without a lock.
//
// Every sample is one relaxed fetch_add on a bucket, plus a few on the
// shard's count, sum, and maximum. Threads are spread over kShards shards, so
// the hot path rarely shares a cache line with another thread. A snapshot
// adds the shards up; it may miss samples being added at that moment, and
// may see a sample's bucket before its sum, minimum, or maximum
// (Histogram::FromBuckets repairs an inconsistent minimum and maximum).
class ConcurrentHistogram {
 public:
  static constexpr std::size_t kShards = kMetricShards;

  void Add(std::uint64_t value) noexcept;
  [[nodiscard]] Histogram Snapshot() const;

 private:
  struct alignas(kCacheLineSize) Shard {
    std::array<std::atomic<std::uint64_t>, Histogram::kBucketCount> buckets{};
    std::atomic<std::uint64_t> sum{0};
    std::atomic<std::uint64_t> min{UINT64_MAX};
    std::atomic<std::uint64_t> max{0};
  };

  std::array<Shard, kShards> shards_{};
};

// Named metrics, registered once and then updated lock-free. Registration
// returns a reference that stays valid for the registry's lifetime, so the
// engine keeps those references and never looks a metric up by name on a hot
// path. Names are dotted and stable: they are the public interface that the
// stats API, the JSON endpoint, and the dashboard all read.
//
// Registration is not thread-safe and must finish before the metrics are
// used; updates and Snapshot are thread-safe.
class MetricsRegistry {
 public:
  Counter& AddCounter(std::string name);
  // Samples are nanoseconds; LatencyValues reports them in microseconds.
  // Recording whole microseconds would turn every sub-microsecond memtable
  // Get into a 0.
  ConcurrentHistogram& AddLatency(std::string name);

  [[nodiscard]] std::map<std::string, std::uint64_t> CounterValues() const;
  [[nodiscard]] std::map<std::string, LatencyStats> LatencyValues() const;

 private:
  // deque: growing it never moves the elements already handed out.
  std::deque<std::pair<std::string, Counter>> counters_;
  std::deque<std::pair<std::string, ConcurrentHistogram>> latencies_;
};

// The summary the stats API reports for a latency histogram, with every
// figure divided by `unit` (1000 turns nanosecond samples into microseconds).
[[nodiscard]] LatencyStats Summarize(const Histogram& histogram, double unit = 1.0);

}  // namespace strata

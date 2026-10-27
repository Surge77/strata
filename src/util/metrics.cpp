#include "util/metrics.hpp"

#include <algorithm>

namespace strata {
namespace {

// Each thread gets a fixed shard the first time it records anything, handed
// out round-robin, so up to kShards threads never share one.
std::size_t ThisThreadShard() noexcept {
  static std::atomic<std::size_t> next{0};
  thread_local const std::size_t shard =
      next.fetch_add(1, std::memory_order_relaxed) % ConcurrentHistogram::kShards;
  return shard;
}

// Lowers `target` to `value` if that is smaller. The plain load first keeps
// the common case -- not a new minimum -- free of read-modify-write traffic.
void AtomicMin(std::atomic<std::uint64_t>* target, std::uint64_t value) noexcept {
  std::uint64_t current = target->load(std::memory_order_relaxed);
  while (value < current &&
         !target->compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

void AtomicMax(std::atomic<std::uint64_t>* target, std::uint64_t value) noexcept {
  std::uint64_t current = target->load(std::memory_order_relaxed);
  while (value > current &&
         !target->compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

}  // namespace

void ConcurrentHistogram::Add(std::uint64_t value) noexcept {
  Shard& shard = shards_[ThisThreadShard()];
  shard.buckets[Histogram::BucketFor(value)].fetch_add(1, std::memory_order_relaxed);
  shard.sum.fetch_add(value, std::memory_order_relaxed);
  AtomicMin(&shard.min, value);
  AtomicMax(&shard.max, value);
}

Histogram ConcurrentHistogram::Snapshot() const {
  std::array<std::uint64_t, Histogram::kBucketCount> buckets{};
  std::uint64_t sum = 0;
  std::uint64_t min = UINT64_MAX;
  std::uint64_t max = 0;
  for (const Shard& shard : shards_) {
    for (std::size_t bucket = 0; bucket < buckets.size(); ++bucket) {
      buckets[bucket] += shard.buckets[bucket].load(std::memory_order_relaxed);
    }
    sum += shard.sum.load(std::memory_order_relaxed);
    min = std::min(min, shard.min.load(std::memory_order_relaxed));
    max = std::max(max, shard.max.load(std::memory_order_relaxed));
  }
  return Histogram::FromBuckets(buckets, min, max, sum);
}

Counter& MetricsRegistry::AddCounter(std::string name) {
  return counters_
      .emplace_back(std::piecewise_construct, std::forward_as_tuple(std::move(name)),
                    std::forward_as_tuple())
      .second;
}

ConcurrentHistogram& MetricsRegistry::AddLatency(std::string name) {
  return latencies_
      .emplace_back(std::piecewise_construct, std::forward_as_tuple(std::move(name)),
                    std::forward_as_tuple())
      .second;
}

std::map<std::string, std::uint64_t> MetricsRegistry::CounterValues() const {
  std::map<std::string, std::uint64_t> values;
  for (const auto& [name, counter] : counters_) {
    values.emplace(name, counter.Value());
  }
  return values;
}

std::map<std::string, LatencyStats> MetricsRegistry::LatencyValues() const {
  std::map<std::string, LatencyStats> values;
  for (const auto& [name, histogram] : latencies_) {
    values.emplace(name, Summarize(histogram.Snapshot()));
  }
  return values;
}

LatencyStats Summarize(const Histogram& histogram) {
  return LatencyStats{
      .count = histogram.Count(),
      .mean = histogram.Mean(),
      .p50 = histogram.Percentile(50),
      .p90 = histogram.Percentile(90),
      .p99 = histogram.Percentile(99),
      .p999 = histogram.Percentile(99.9),
      .max = histogram.Max(),
  };
}

}  // namespace strata

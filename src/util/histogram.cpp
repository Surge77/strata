#include "util/histogram.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>

namespace strata {
namespace {

// Values below 2^kSubBucketBits are exact; each power of two above that is
// split into 2^kSubBucketBits sub-buckets.
constexpr unsigned kSubBucketBits = 4;
constexpr std::uint64_t kSubBuckets = 1U << kSubBucketBits;  // 16

}  // namespace

std::size_t Histogram::BucketFor(std::uint64_t value) noexcept {
  if (value < kSubBuckets) {
    return value;
  }
  // `exponent` is the index of the highest set bit (>= 4 here). The four bits
  // below it select the sub-bucket, so every bucket in this power-of-two range
  // is 2^(exponent - 4) wide: 1/16 of the range's lower bound.
  const auto exponent = static_cast<unsigned>(std::bit_width(value)) - 1U;
  const unsigned shift = exponent - kSubBucketBits;
  const std::uint64_t sub_bucket = (value >> shift) & (kSubBuckets - 1);
  // size_t and uint64_t are the same type on the 64-bit Linux targets this
  // engine supports, so no cast is needed (and GCC's -Wuseless-cast rejects one).
  return kSubBuckets + (exponent - kSubBucketBits) * kSubBuckets + sub_bucket;
}

std::uint64_t Histogram::BucketLowerBound(std::size_t bucket) noexcept {
  if (bucket < kSubBuckets) {
    return bucket;
  }
  const std::size_t offset = bucket - kSubBuckets;
  const auto shift = static_cast<unsigned>(offset / kSubBuckets);
  const std::uint64_t sub_bucket = offset % kSubBuckets;
  return (kSubBuckets + sub_bucket) << shift;
}

std::uint64_t Histogram::BucketUpperBound(std::size_t bucket) noexcept {
  if (bucket + 1 >= kBucketCount) {
    return UINT64_MAX;
  }
  return BucketLowerBound(bucket + 1);
}

void Histogram::Add(std::uint64_t value) noexcept {
  ++buckets_[BucketFor(value)];
  ++count_;
  min_ = std::min(min_, value);
  max_ = std::max(max_, value);
  const auto as_double = static_cast<double>(value);
  const double delta = as_double - mean_;
  mean_ += delta / static_cast<double>(count_);
  squared_deviations_ += delta * (as_double - mean_);
}

void Histogram::Merge(const Histogram& other) noexcept {
  for (std::size_t bucket = 0; bucket < kBucketCount; ++bucket) {
    buckets_[bucket] += other.buckets_[bucket];
  }
  if (other.count_ == 0) {
    return;
  }
  // Chan et al.'s pairwise combination of two Welford states.
  const auto left = static_cast<double>(count_);
  const auto right = static_cast<double>(other.count_);
  const double total = left + right;
  const double delta = other.mean_ - mean_;
  mean_ += delta * right / total;
  squared_deviations_ += other.squared_deviations_ + delta * delta * left * right / total;
  count_ += other.count_;
  min_ = std::min(min_, other.min_);
  max_ = std::max(max_, other.max_);
}

void Histogram::Clear() noexcept {
  *this = Histogram();
}

double Histogram::Mean() const noexcept {
  return mean_;
}

double Histogram::StandardDeviation() const noexcept {
  if (count_ == 0) {
    return 0.0;
  }
  return std::sqrt(squared_deviations_ / static_cast<double>(count_));
}

double Histogram::Percentile(double percent) const noexcept {
  if (count_ == 0) {
    return 0.0;
  }
  if (percent <= 0.0) {
    return static_cast<double>(min_);
  }
  if (percent >= 100.0) {
    return static_cast<double>(max_);
  }

  const double target_rank = static_cast<double>(count_) * percent / 100.0;
  double seen = 0.0;
  for (std::size_t bucket = 0; bucket < kBucketCount; ++bucket) {
    const auto in_bucket = static_cast<double>(buckets_[bucket]);
    if (seen + in_bucket < target_rank) {
      seen += in_bucket;
      continue;
    }
    // Samples are assumed spread evenly across the bucket's range. The result
    // is clamped to [min, max], which makes the extreme buckets exact.
    const auto lower = static_cast<double>(BucketLowerBound(bucket));
    const auto upper = static_cast<double>(BucketUpperBound(bucket));
    const double fraction = (target_rank - seen) / in_bucket;
    const double estimate = lower + (upper - lower) * fraction;
    return std::clamp(estimate, static_cast<double>(min_), static_cast<double>(max_));
  }
  return static_cast<double>(max_);
}

std::string Histogram::ToString() const {
  return std::format(
      "count={} mean={:.1f} stddev={:.1f} min={} p50={:.1f} p90={:.1f} p99={:.1f} "
      "p99.9={:.1f} max={}",
      count_, Mean(), StandardDeviation(), Min(), Percentile(50), Percentile(90), Percentile(99),
      Percentile(99.9), max_);
}

}  // namespace strata

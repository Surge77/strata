#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace strata {

// A fixed-memory distribution of non-negative integer samples, typically
// latencies in microseconds.
//
// Buckets are log-linear: values below 16 each get an exact bucket, and every
// power-of-two range above that is split into 16 equal sub-buckets. The
// relative width of any bucket is therefore at most 1/16, so a percentile
// read from it is within 6.25% of the true sample value -- at 3 microseconds
// or at 3 seconds alike. Count, sum, min, max, mean, and standard deviation are
// kept exactly.
//
// Memory is constant (976 counters) however many samples are added, and two
// histograms merge by adding counters, which lets each thread record into its
// own histogram and combine them afterwards. Not thread-safe.
class Histogram {
 public:
  void Add(std::uint64_t value) noexcept;
  void Merge(const Histogram& other) noexcept;
  void Clear() noexcept;

  [[nodiscard]] std::uint64_t Count() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t Min() const noexcept { return count_ == 0 ? 0 : min_; }
  [[nodiscard]] std::uint64_t Max() const noexcept { return max_; }
  [[nodiscard]] double Mean() const noexcept;
  [[nodiscard]] double StandardDeviation() const noexcept;

  // The value below which `percent` percent of samples fall, for `percent` in
  // [0, 100]. Percentile(0) is Min() and Percentile(100) is Max(); an empty
  // histogram returns 0.
  [[nodiscard]] double Percentile(double percent) const noexcept;

  // "count=1000 mean=12.4 stddev=3.1 min=2 p50=11.8 p90=16.0 p99=24.7 max=31"
  [[nodiscard]] std::string ToString() const;

  // Exposed for tests: the bucket a value lands in and that bucket's range.
  static constexpr std::size_t kBucketCount = 976;
  [[nodiscard]] static std::size_t BucketFor(std::uint64_t value) noexcept;
  [[nodiscard]] static std::uint64_t BucketLowerBound(std::size_t bucket) noexcept;
  // Exclusive, saturating at UINT64_MAX for the top bucket.
  [[nodiscard]] static std::uint64_t BucketUpperBound(std::size_t bucket) noexcept;

 private:
  std::array<std::uint64_t, kBucketCount> buckets_{};
  std::uint64_t count_ = 0;
  std::uint64_t min_ = UINT64_MAX;
  std::uint64_t max_ = 0;
  // Sums are kept in floating point: a sum of squared microsecond latencies
  // overflows 64 bits after a few million multi-second samples.
  double sum_ = 0.0;
  double sum_of_squares_ = 0.0;
};

}  // namespace strata

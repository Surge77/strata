#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace strata::bench {

// The statistical protocol of docs/benchmarks.md: warm-up rounds thrown away,
// at least kMinPublishableRounds measured, reported as the median with its
// coefficient of variation, and stable only when that is at most kMaxStableCv.
inline constexpr double kMaxStableCv = 0.05;
inline constexpr int kMinPublishableRounds = 5;

struct Summary {
  double median = 0;
  double mean = 0;
  double min = 0;
  double max = 0;
  // Sample standard deviation (n - 1) over the mean. 0 for fewer than two
  // rounds, where spread cannot be measured, and for a zero mean.
  double cv = 0;
  std::size_t rounds = 0;

  [[nodiscard]] bool stable() const noexcept { return cv <= kMaxStableCv; }
};

[[nodiscard]] Summary Summarize(std::span<const double> values);

struct Protocol {
  int warmup = 1;
  int rounds = kMinPublishableRounds;

  [[nodiscard]] bool publishable() const noexcept { return rounds >= kMinPublishableRounds; }
};

// Calls `measure` warmup + rounds times and returns the measured rounds'
// values in order, warm-up discarded. Negative counts are treated as zero.
template <typename Measure>
[[nodiscard]] std::vector<double> RunRounds(const Protocol& protocol, Measure&& measure) {
  for (int round = 0; round < protocol.warmup; ++round) {
    static_cast<void>(measure());
  }
  std::vector<double> values;
  for (int round = 0; round < protocol.rounds; ++round) {
    values.push_back(measure());
  }
  return values;
}

}  // namespace strata::bench

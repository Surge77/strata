#pragma once

#include <chrono>
#include <vector>

#include "stratabench/stats.hpp"
#include "stratabench/workloads.hpp"

namespace strata::bench {

struct LatencyConfig {
  double read_fraction = 0.9;  // the rest are Puts of existing keys
  int workers = 4;
  std::chrono::nanoseconds duration = std::chrono::seconds(5);  // per round
  std::vector<double> rates;  // offered ops/s; empty means 25/50/75/90% of max
};

inline constexpr double kAutoRateFractions[] = {0.25, 0.50, 0.75, 0.90};

// One offered rate, summarised over the protocol's measured rounds. Latencies
// are in microseconds.
struct LatencyRow {
  double offered_rate = 0;
  Summary achieved_rate;
  Summary response_p50;  // gated
  Summary response_p99;
  Summary response_p999;
  Summary response_max;
  Summary service_p50;
  Summary service_p99;
  int saturated_rounds = 0;  // a row with any is labelled queue-dominated
};

struct LatencyReport {
  bool ran = false;
  double read_fraction = 0;
  int workers = 0;
  double duration_seconds = 0;
  Summary closed_loop_max{};  // ops/s with `workers` threads, same mix
  std::vector<LatencyRow> rows{};
  LatencyRow harness_floor{};  // the no-op operation at the highest rate
};

// The open-loop measurement of docs/benchmarks.md against the loaded database.
[[nodiscard]] LatencyReport RunLatency(const BenchConfig& config, const LatencyConfig& latency,
                                       LoadedDb* loaded);

}  // namespace strata::bench

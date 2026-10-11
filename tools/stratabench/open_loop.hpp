#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include "util/histogram.hpp"

namespace strata::bench {

// Arrival times, as offsets from the start of a run, for a Poisson process of
// `rate` arrivals per second over `duration`: the gaps are exponentially
// distributed with mean 1 / rate. The same seed gives the same schedule.
[[nodiscard]] std::vector<std::chrono::nanoseconds> PoissonSchedule(
    double rate, std::chrono::nanoseconds duration, std::uint64_t seed);

struct OpenLoopConfig {
  double rate = 0;  // offered arrivals per second
  std::chrono::nanoseconds duration{0};
  int workers = 4;
  std::uint64_t seed = 1;
};

struct OpenLoopResult {
  // From each operation's *scheduled* time to its completion: includes time
  // spent queued behind earlier operations. This is the latency a client sees.
  Histogram response_ns;
  // From each operation's actual start to its completion.
  Histogram service_ns;
  std::uint64_t operations = 0;
  double elapsed_seconds = 0;  // run start to the last completion
  double offered_rate = 0;     // operations / duration
  double achieved_rate = 0;    // operations / elapsed_seconds
  // The workers fell behind: achieved_rate < kSaturationRatio * offered_rate.
  // Latencies of a saturated run measure the queue, not the operation.
  bool saturated = false;
};

inline constexpr double kSaturationRatio = 0.98;

// Runs `op(index)` once for every arrival in PoissonSchedule(config...), from
// `config.workers` threads. Arrivals are taken in order; a worker sleeps until
// its arrival's scheduled time unless that time has passed. Response times are
// measured from the scheduled time, so a slow operation is charged for the
// arrivals that waited behind it: there is no coordinated omission. `op` must
// be safe to call from several threads at once.
[[nodiscard]] OpenLoopResult RunOpenLoop(const OpenLoopConfig& config,
                                         const std::function<void(std::uint64_t)>& op);

}  // namespace strata::bench

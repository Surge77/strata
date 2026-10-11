#include "stratabench/open_loop.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

#include <sys/prctl.h>

#include "stratabench/key_generator.hpp"

namespace strata::bench {
namespace {

using Clock = std::chrono::steady_clock;

// Workers start this long after the schedule is fixed, so every thread is
// running before the first arrival is due.
constexpr auto kStartLead = std::chrono::milliseconds(10);

// Linux delays a sleeping thread's wake-up by up to its timer slack, 50 us by
// default, to batch timer interrupts. That delay would be charged to every
// operation's response time, so workers ask for the minimum.
constexpr unsigned long kTimerSlackNanos = 1;

struct WorkerTotals {
  Histogram response_ns;
  Histogram service_ns;
  Clock::time_point last_end{};
};

std::uint64_t NanosBetween(Clock::time_point from, Clock::time_point to) {
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count();
  return nanos > 0 ? static_cast<std::uint64_t>(nanos) : 0;
}

}  // namespace

std::vector<std::chrono::nanoseconds> PoissonSchedule(double rate,
                                                      std::chrono::nanoseconds duration,
                                                      std::uint64_t seed) {
  std::vector<std::chrono::nanoseconds> schedule;
  if (rate <= 0 || duration <= std::chrono::nanoseconds::zero()) {
    return schedule;
  }
  schedule.reserve(
      static_cast<std::size_t>(rate * std::chrono::duration<double>(duration).count()));
  const double mean_gap_nanos = 1e9 / rate;
  Random random(seed);
  double at = 0;
  const auto end = static_cast<double>(duration.count());
  while (true) {
    // Inverse-CDF sampling of an exponential gap; 1 - u is in (0, 1], so the
    // logarithm is finite.
    at += -std::log(1.0 - UniformDouble(&random)) * mean_gap_nanos;
    if (at >= end) {
      break;
    }
    schedule.emplace_back(static_cast<std::int64_t>(at));
  }
  return schedule;
}

OpenLoopResult RunOpenLoop(const OpenLoopConfig& config,
                           const std::function<void(std::uint64_t)>& op) {
  const std::vector<std::chrono::nanoseconds> schedule =
      PoissonSchedule(config.rate, config.duration, config.seed);
  const int workers = std::max(config.workers, 1);
  std::vector<WorkerTotals> totals(static_cast<std::size_t>(workers));
  std::atomic<std::size_t> next{0};
  const Clock::time_point start = Clock::now() + kStartLead;

  std::vector<std::thread> threads;
  for (int worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&, worker] {
      static_cast<void>(prctl(PR_SET_TIMERSLACK, kTimerSlackNanos));
      WorkerTotals& mine = totals[static_cast<std::size_t>(worker)];
      for (std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
           index < schedule.size(); index = next.fetch_add(1, std::memory_order_relaxed)) {
        const Clock::time_point scheduled = start + schedule[index];
        std::this_thread::sleep_until(scheduled);
        const Clock::time_point began = Clock::now();
        op(index);
        const Clock::time_point ended = Clock::now();
        mine.response_ns.Add(NanosBetween(scheduled, ended));
        mine.service_ns.Add(NanosBetween(began, ended));
        mine.last_end = ended;
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  OpenLoopResult result;
  Clock::time_point last_end = start;
  for (const WorkerTotals& worker : totals) {
    result.response_ns.Merge(worker.response_ns);
    result.service_ns.Merge(worker.service_ns);
    last_end = std::max(last_end, worker.last_end);
  }
  result.operations = schedule.size();
  if (result.operations == 0) {
    return result;
  }
  result.elapsed_seconds = std::chrono::duration<double>(last_end - start).count();
  result.offered_rate = static_cast<double>(result.operations) /
                        std::chrono::duration<double>(config.duration).count();
  result.achieved_rate = result.elapsed_seconds > 0
                             ? static_cast<double>(result.operations) / result.elapsed_seconds
                             : result.offered_rate;
  // The schedule ends at the last arrival, slightly before `duration`, so a
  // run that keeps up finishes a little early and its achieved rate can exceed
  // the offered one; only falling short means the workers could not keep up.
  result.saturated = result.achieved_rate < kSaturationRatio * result.offered_rate;
  return result;
}

}  // namespace strata::bench

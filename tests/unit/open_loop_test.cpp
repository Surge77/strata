#include "stratabench/open_loop.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using strata::bench::OpenLoopConfig;
using strata::bench::OpenLoopResult;
using strata::bench::PoissonSchedule;
using strata::bench::RunOpenLoop;

constexpr double kNanosPerMilli = 1e6;

TEST(OpenLoop, PoissonScheduleIsSortedAndInsideTheWindow) {
  const auto schedule = PoissonSchedule(10'000, 2s, /*seed=*/1);
  ASSERT_FALSE(schedule.empty());
  EXPECT_TRUE(std::is_sorted(schedule.begin(), schedule.end()));
  EXPECT_GE(schedule.front(), 0ns);
  EXPECT_LT(schedule.back(), 2s);
}

// 20,000 expected arrivals; a Poisson count has standard deviation sqrt(20,000)
// = 141, so +/- 600 is more than 4 sigma.
TEST(OpenLoop, PoissonScheduleHasTheRequestedRate) {
  const auto schedule = PoissonSchedule(10'000, 2s, /*seed=*/2);
  EXPECT_NEAR(static_cast<double>(schedule.size()), 20'000.0, 600.0);
}

TEST(OpenLoop, PoissonScheduleIsReproducibleFromItsSeed) {
  EXPECT_EQ(PoissonSchedule(5'000, 500ms, 3), PoissonSchedule(5'000, 500ms, 3));
  EXPECT_NE(PoissonSchedule(5'000, 500ms, 3), PoissonSchedule(5'000, 500ms, 4));
}

TEST(OpenLoop, ZeroRateOrDurationGivesAnEmptySchedule) {
  EXPECT_TRUE(PoissonSchedule(0, 1s, 1).empty());
  EXPECT_TRUE(PoissonSchedule(1000, 0s, 1).empty());
}

TEST(OpenLoop, RunsEveryScheduledOperationExactlyOnce) {
  const OpenLoopConfig config{.rate = 4'000, .duration = 250ms, .workers = 3, .seed = 5};
  const std::size_t expected = PoissonSchedule(config.rate, config.duration, config.seed).size();
  std::vector<std::atomic<int>> runs(expected);
  const OpenLoopResult result = RunOpenLoop(
      config, [&](std::uint64_t index) { runs[index].fetch_add(1, std::memory_order_relaxed); });
  EXPECT_EQ(result.operations, expected);
  EXPECT_EQ(result.response_ns.Count(), expected);
  EXPECT_EQ(result.service_ns.Count(), expected);
  for (const auto& count : runs) {
    ASSERT_EQ(count.load(), 1);
  }
  EXPECT_FALSE(result.saturated);
  EXPECT_NEAR(result.achieved_rate, result.offered_rate, 0.05 * result.offered_rate);
}

// The point of the harness. One operation stalls for 200 ms with one worker.
// Arrivals during the stall wait for it, and their response times -- measured
// from when they were scheduled -- show that. Their service times do not, which
// is what a closed-loop benchmark would have reported as the latency.
TEST(OpenLoop, AStallShowsUpInResponseTimeButNotServiceTime) {
  const OpenLoopConfig config{.rate = 2'000, .duration = 1s, .workers = 1, .seed = 6};
  constexpr std::uint64_t kStalledOp = 100;
  const OpenLoopResult result = RunOpenLoop(config, [&](std::uint64_t index) {
    if (index == kStalledOp) {
      std::this_thread::sleep_for(200ms);
    }
  });
  // About 400 of about 2,000 arrivals land inside the stall; roughly 300 of
  // those wait more than 50 ms, so the 90th percentile is above 50 ms.
  EXPECT_GT(result.response_ns.Percentile(90), 50 * kNanosPerMilli);
  EXPECT_GE(result.response_ns.Max(), 150 * kNanosPerMilli);
  // Only the stalled operation itself took long to serve.
  EXPECT_LT(result.service_ns.Percentile(99.9), 50 * kNanosPerMilli);
  EXPECT_GE(result.service_ns.Max(), 200 * kNanosPerMilli);
}

TEST(OpenLoop, ASlowOperationSaturatesTheRun) {
  const OpenLoopConfig config{.rate = 2'000, .duration = 200ms, .workers = 1, .seed = 7};
  // 1 ms per operation caps one worker near 1,000 op/s, half the offered rate.
  const OpenLoopResult result =
      RunOpenLoop(config, [](std::uint64_t) { std::this_thread::sleep_for(1ms); });
  EXPECT_TRUE(result.saturated);
  EXPECT_LT(result.achieved_rate, 0.98 * result.offered_rate);
}

TEST(OpenLoop, EmptyScheduleRunsNothing) {
  int calls = 0;
  const OpenLoopResult result = RunOpenLoop({.rate = 0, .duration = 1s, .workers = 2, .seed = 1},
                                            [&](std::uint64_t) { ++calls; });
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(result.operations, 0U);
  EXPECT_FALSE(result.saturated);
}

}  // namespace

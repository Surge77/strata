// The soak's growth verdict (tools/stratasoak/monitor.cpp), on made-up samples.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "monitor.hpp"

namespace strata::soak {
namespace {

constexpr std::uint64_t kMiB = 1 << 20;
constexpr std::size_t kWriteBuffer = 4 * kMiB;

// 40 samples a minute apart, with `rss` and `fds` given as functions of the
// sample's index.
template <typename Rss, typename Fds>
std::vector<ResourceSample> MakeSamples(Rss rss, Fds fds, std::uint64_t memtable = kMiB) {
  std::vector<ResourceSample> samples;
  for (int i = 0; i < 40; ++i) {
    samples.push_back({.seconds = 60.0 * i,
                       .rss_bytes = rss(i),
                       .open_fds = fds(i),
                       .memtable_bytes = memtable,
                       .immutable_bytes = 0,
                       .table_files = 20});
  }
  return samples;
}

bool Passed(const std::vector<Check>& checks, std::size_t index) {
  return index < checks.size() && checks[index].passed;
}

TEST(SoakMonitorTest, AFlatRunAfterWarmUpPasses) {
  // Memory climbs while caches fill in the first quarter, then holds.
  const auto checks =
      CheckGrowth(MakeSamples(
                      [](int i) -> std::uint64_t {
                        return static_cast<std::uint64_t>(i < 10 ? 50 + 5 * i : 100) * kMiB;
                      },
                      [](int) -> std::uint64_t { return 40; }),
                  {.write_buffer_size = kWriteBuffer});
  ASSERT_EQ(checks.size(), 3U);
  for (const Check& check : checks) {
    EXPECT_TRUE(check.passed) << check.name << ": " << check.measured << " " << check.bound;
  }
}

TEST(SoakMonitorTest, MemoryThatKeepsGrowingFails) {
  // 2 MiB a minute: the last quarter ends far above the second.
  const auto checks = CheckGrowth(
      MakeSamples(
          [](int i) -> std::uint64_t { return (100 + 2 * static_cast<std::uint64_t>(i)) * kMiB; },
          [](int) -> std::uint64_t { return 40; }),
      {.write_buffer_size = kWriteBuffer});
  EXPECT_FALSE(Passed(checks, 0)) << checks[0].measured;
  EXPECT_TRUE(Passed(checks, 1));
}

TEST(SoakMonitorTest, DescriptorsThatKeepGrowingFail) {
  const auto checks = CheckGrowth(
      MakeSamples([](int) -> std::uint64_t { return 100 * kMiB; },
                  [](int i) -> std::uint64_t { return 40 + static_cast<std::uint64_t>(i); }),
      {.write_buffer_size = kWriteBuffer});
  EXPECT_TRUE(Passed(checks, 0));
  EXPECT_FALSE(Passed(checks, 1)) << checks[1].measured;
}

TEST(SoakMonitorTest, AMemtablePastTwiceTheWriteBufferFails) {
  const auto checks =
      CheckGrowth(MakeSamples([](int) -> std::uint64_t { return 100 * kMiB; },
                              [](int) -> std::uint64_t { return 40; }, 3 * kWriteBuffer),
                  {.write_buffer_size = kWriteBuffer});
  EXPECT_FALSE(Passed(checks, 2)) << checks[2].measured;
}

TEST(SoakMonitorTest, TooFewSamplesIsAFailureNotAPass) {
  const std::vector<ResourceSample> few(5, ResourceSample{});
  const auto checks = CheckGrowth(few, {.write_buffer_size = kWriteBuffer});
  ASSERT_EQ(checks.size(), 1U);
  EXPECT_FALSE(checks[0].passed);
}

}  // namespace
}  // namespace strata::soak

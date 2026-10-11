// Round timing for the closed-loop workloads: thread release, time-bounded
// rounds, and operation shares. Declared in workloads.hpp.

#include <algorithm>
#include <chrono>
#include <latch>
#include <numeric>
#include <thread>

#include "stratabench/workloads.hpp"

namespace strata::bench {
namespace {

using Clock = std::chrono::steady_clock;

// A timed round checks its deadline once per chunk, not once per operation.
constexpr std::uint64_t kChunkOps = 256;

}  // namespace

double TimeThreads(int threads, const std::function<void(int)>& body) {
  const int count = std::max(threads, 1);
  std::latch ready(count);
  std::latch go(1);
  std::vector<std::thread> workers;
  for (int thread = 0; thread < count; ++thread) {
    workers.emplace_back([&, thread] {
      ready.count_down();
      go.wait();
      body(thread);
    });
  }
  ready.wait();
  const Clock::time_point start = Clock::now();
  go.count_down();
  for (auto& worker : workers) {
    worker.join();
  }
  return std::chrono::duration<double>(Clock::now() - start).count();
}

Timed RunForDuration(int threads, std::chrono::nanoseconds min_round, const ChunkFactory& factory) {
  const int count = std::max(threads, 1);
  std::vector<ChunkFn> chunks;
  for (int thread = 0; thread < count; ++thread) {
    chunks.push_back(factory(thread));  // built before the clock starts
  }
  std::vector<std::uint64_t> done(static_cast<std::size_t>(count), 0);
  const double seconds = TimeThreads(count, [&](int thread) {
    const auto slot = static_cast<std::size_t>(thread);
    const Clock::time_point deadline = Clock::now() + min_round;
    std::uint64_t ops = 0;
    do {
      chunks[slot](ops, kChunkOps);
      ops += kChunkOps;
    } while (Clock::now() < deadline);
    done[slot] = ops;
  });
  return {.operations = std::accumulate(done.begin(), done.end(), std::uint64_t{0}),
          .seconds = seconds};
}

Share ShareOf(std::uint64_t total, int thread, int threads) noexcept {
  const auto count = static_cast<std::uint64_t>(std::max(threads, 1));
  const auto index = static_cast<std::uint64_t>(thread);
  return {.begin = total * index / count, .end = total * (index + 1) / count};
}
}  // namespace strata::bench

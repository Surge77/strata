// Phase 9 gate: how writes scale with threads, and whether reads keep moving
// while writers wait on fsync.
//
//   write    unsynced Puts, total ops/s for 1, 2, 4, 8 writer threads
//   sync     synced Puts (each acknowledged only once durable), same threads
//   readers  Get ops/s from 1 reader while 0, 1, 4, 8 threads do synced Puts
//
// Each cell is the median of `rounds` runs on a fresh database, reported with
// its coefficient of variation. The database lives on the scratch directory,
// which should be on the filesystem being measured (ext4 here).
//
// Usage: concurrency_bench <scratch-dir> [rounds]

#include "strata/db.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kValueSize = 100;
constexpr int kUnsyncedOpsTotal = 200'000;
constexpr int kSyncedOpsPerThread = 400;
constexpr int kReaderKeys = 10'000;
constexpr auto kReaderWindow = std::chrono::milliseconds(1500);
constexpr int kDefaultRounds = 3;
constexpr int kThreadCounts[] = {1, 2, 4, 8};
constexpr int kReaderWriterCounts[] = {0, 1, 4, 8};

[[noreturn]] void Fail(const char* what, const strata::Status& status) {
  std::fprintf(stderr, "%s: %s\n", what, status.ToString().c_str());
  std::exit(1);
}

std::string Key(int thread, int index) {
  return "t" + std::to_string(thread) + "-" + std::to_string(1'000'000 + index);
}

strata::DB OpenFresh(const std::filesystem::path& path) {
  std::filesystem::remove_all(path);
  strata::DB db;
  if (const auto status = strata::DB::Open({.path = path.string()}, &db); !status.ok()) {
    Fail("open", status);
  }
  return db;
}

// Total write ops/s with `threads` writers sharing `total_ops`.
double MeasureWrites(const std::filesystem::path& path, int threads, int ops_per_thread,
                     bool sync) {
  strata::DB db = OpenFresh(path);
  const std::string value(kValueSize, 'v');
  std::vector<std::thread> workers;
  const auto start = Clock::now();
  for (int thread = 0; thread < threads; ++thread) {
    workers.emplace_back([&, thread] {
      for (int index = 0; index < ops_per_thread; ++index) {
        if (const auto s = db.Put(Key(thread, index), value, {.sync = sync}); !s.ok()) {
          Fail("put", s);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  static_cast<void>(db.Close());
  return static_cast<double>(threads) * ops_per_thread / seconds;
}

// Get ops/s from one reader while `writers` threads do synced Puts.
double MeasureReader(const std::filesystem::path& path, int writers) {
  strata::DB db = OpenFresh(path);
  const std::string value(kValueSize, 'v');
  for (int index = 0; index < kReaderKeys; ++index) {
    if (const auto s = db.Put(Key(99, index), value); !s.ok()) {
      Fail("preload", s);
    }
  }
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  for (int thread = 0; thread < writers; ++thread) {
    workers.emplace_back([&, thread] {
      for (int index = 0; !stop.load(std::memory_order_relaxed); ++index) {
        if (const auto s = db.Put(Key(thread, index), value, {.sync = true}); !s.ok()) {
          Fail("writer put", s);
        }
      }
    });
  }
  std::uint64_t reads = 0;
  const auto start = Clock::now();
  while (Clock::now() - start < kReaderWindow) {
    if (const auto r = db.Get(Key(99, static_cast<int>(reads % kReaderKeys))); !r.ok()) {
      Fail("get", r.status());
    }
    ++reads;
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  stop = true;
  for (auto& worker : workers) {
    worker.join();
  }
  static_cast<void>(db.Close());
  return static_cast<double>(reads) / seconds;
}

struct Summary {
  double median = 0;
  double cv = 0;
};

template <typename Measure>
Summary Repeat(int rounds, Measure measure) {
  std::vector<double> values;
  for (int round = 0; round < rounds; ++round) {
    values.push_back(measure());
  }
  std::vector<double> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  double mean = 0;
  for (const double value : values) {
    mean += value;
  }
  mean /= static_cast<double>(values.size());
  double variance = 0;
  for (const double value : values) {
    variance += (value - mean) * (value - mean);
  }
  variance /= static_cast<double>(values.size());
  return {.median = sorted[sorted.size() / 2], .cv = mean == 0 ? 0 : std::sqrt(variance) / mean};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <scratch-dir> [rounds]\n", argv[0]);
    return 2;
  }
  const std::filesystem::path path = std::filesystem::path(argv[1]) / "concurrency_bench";
  const int rounds = argc > 2 ? std::max(1, std::atoi(argv[2])) : kDefaultRounds;
  std::printf("rounds=%d value=%zuB hardware_threads=%u\n", rounds, kValueSize,
              std::thread::hardware_concurrency());

  std::printf("\n%-8s %14s %7s %14s %7s\n", "threads", "unsynced op/s", "CV", "synced op/s", "CV");
  for (const int threads : kThreadCounts) {
    const Summary unsynced = Repeat(rounds, [&] {
      return MeasureWrites(path, threads, kUnsyncedOpsTotal / threads, /*sync=*/false);
    });
    const Summary synced = Repeat(
        rounds, [&] { return MeasureWrites(path, threads, kSyncedOpsPerThread, /*sync=*/true); });
    std::printf("%-8d %14.0f %6.1f%% %14.0f %6.1f%%\n", threads, unsynced.median, 100 * unsynced.cv,
                synced.median, 100 * synced.cv);
  }

  std::printf("\n%-16s %14s %7s\n", "synced writers", "reader Get/s", "CV");
  for (const int writers : kReaderWriterCounts) {
    const Summary reader = Repeat(rounds, [&] { return MeasureReader(path, writers); });
    std::printf("%-16d %14.0f %6.1f%%\n", writers, reader.median, 100 * reader.cv);
  }
  std::filesystem::remove_all(path);
  return 0;
}

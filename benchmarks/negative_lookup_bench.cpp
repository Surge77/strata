// Phase 6 gate: the cost of Get for keys that are not in the database, with
// the per-table Bloom filter on and off, and with the block cache on and off.
//
// Workload: `keys` even-numbered keys are written in a shuffled order with a
// small write buffer, so the database ends up as many level-0 tables that
// each span the whole key range -- the case a filter exists for, because a
// missing key falls inside every table's range. Lookups are odd-numbered
// keys, so every one misses. Each database is reopened before measuring.
//
// Method:
// - All four databases are built first, then measured in interleaved rounds
//   (config 1, 2, 3, 4, 1, 2, ...) so machine drift hits every config alike.
// - Each round has a throughput pass, timed once around the whole batch with
//   no clock reads inside it, and a separate latency pass that times every
//   lookup. The latency percentiles include one clock-read pair, whose cost
//   is measured and printed.
// - Every statistic in a row is pooled over all rounds: the mean is the
//   median of per-round means, the percentiles come from one histogram fed by
//   every round, and reads per lookup is total reads over total lookups.
// - The OS page cache is warm, so "block cache off" means a pread served from
//   memory, not a disk access. The gains are a CPU and syscall saving; with
//   cold storage the filter's benefit would be larger.
//
// Usage: negative_lookup_bench <scratch-dir> [keys] [lookups] [rounds]

#include "strata/db.hpp"
#include "strata/env.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "util/histogram.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kWriteBuffer = 1024 * 1024;
constexpr std::size_t kValueSize = 100;
constexpr std::size_t kBlockCacheBytes = 8 * 1024 * 1024;
constexpr std::uint64_t kSeed = 20261004;
constexpr std::size_t kDefaultKeys = 200'000;
constexpr std::size_t kDefaultLookups = 100'000;
constexpr std::size_t kDefaultRounds = 5;
constexpr std::size_t kWarmupDivisor = 10;
constexpr int kClockSamples = 100'000;

class CountingFile final : public strata::RandomAccessFile {
 public:
  CountingFile(std::unique_ptr<strata::RandomAccessFile> file, std::atomic<std::uint64_t>* reads)
      : file_(std::move(file)), reads_(reads) {}

  strata::Result<strata::Slice> Read(std::uint64_t offset, std::size_t n,
                                     char* scratch) const override {
    reads_->fetch_add(1, std::memory_order_relaxed);
    return file_->Read(offset, n, scratch);
  }

 private:
  std::unique_ptr<strata::RandomAccessFile> file_;
  std::atomic<std::uint64_t>* reads_;
};

// Counts SSTable reads so the report can show *why* a configuration is fast.
class CountingEnv final : public strata::EnvWrapper {
 public:
  CountingEnv() : EnvWrapper(strata::Env::Default()) {}

  strata::Result<std::unique_ptr<strata::RandomAccessFile>> NewRandomAccessFile(
      const std::string& path) override {
    STRATA_ASSIGN_OR_RETURN(auto file, target()->NewRandomAccessFile(path));
    if (path.ends_with(".sst")) {
      return std::unique_ptr<strata::RandomAccessFile>(
          std::make_unique<CountingFile>(std::move(file), &reads_));
    }
    return file;
  }

  std::uint64_t TakeReads() { return reads_.exchange(0); }

 private:
  std::atomic<std::uint64_t> reads_{0};
};

std::string Key(std::uint64_t number) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "key%016llu", static_cast<unsigned long long>(number));
  return buffer;
}

[[noreturn]] void Fail(const std::string& what, const strata::Status& status) {
  std::fprintf(stderr, "%s: %s\n", what.c_str(), status.ToString().c_str());
  std::exit(1);
}

// One configuration under test, with its own database and counters.
struct Bench {
  const char* name;
  bool bloom_filter;
  std::size_t block_cache_size;
  std::filesystem::path path;
  CountingEnv env;
  strata::DB db;
  std::vector<double> round_means_ns;
  strata::Histogram latency_ns;
  std::uint64_t reads = 0;
  std::uint64_t lookups = 0;

  [[nodiscard]] strata::Options Options() {
    return {.path = path.string(),
            .write_buffer_size = kWriteBuffer,
            .bloom_filter = bloom_filter,
            .block_cache_size = block_cache_size,
            .env = &env};
  }
};

std::uint64_t Nanos(Clock::duration duration) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

std::size_t CountTables(const std::filesystem::path& db_path) {
  std::size_t tables = 0;
  for (const auto& entry : std::filesystem::directory_iterator(db_path)) {
    if (entry.path().extension() == ".sst") {
      ++tables;
    }
  }
  return tables;
}

void Load(Bench* bench, std::size_t keys) {
  std::filesystem::remove_all(bench->path);
  std::vector<std::uint64_t> order(keys);
  for (std::size_t index = 0; index < keys; ++index) {
    order[index] = 2 * static_cast<std::uint64_t>(index);
  }
  std::mt19937_64 random(kSeed);
  std::shuffle(order.begin(), order.end(), random);
  strata::DB db;
  if (const auto status = strata::DB::Open(bench->Options(), &db); !status.ok()) {
    Fail("open for load", status);
  }
  const std::string value(kValueSize, 'v');
  for (const std::uint64_t number : order) {
    if (const auto status = db.Put(Key(number), value); !status.ok()) {
      Fail("put", status);
    }
  }
  if (const auto status = db.Close(); !status.ok()) {
    Fail("close after load", status);
  }
  if (const auto status = strata::DB::Open(bench->Options(), &bench->db); !status.ok()) {
    Fail("reopen", status);
  }
}

void Miss(strata::DB* db, const std::string& key) {
  const auto result = db->Get(key);
  if (!result.status().IsNotFound()) {
    Fail("lookup of " + key + " should miss", result.status());
  }
}

void MeasureRound(Bench* bench, const std::vector<std::string>& lookups) {
  const std::size_t warmup = lookups.size() / kWarmupDivisor;
  for (std::size_t index = 0; index < warmup; ++index) {
    Miss(&bench->db, lookups[index]);
  }
  static_cast<void>(bench->env.TakeReads());

  const auto start = Clock::now();
  for (const std::string& key : lookups) {
    Miss(&bench->db, key);
  }
  const auto elapsed = Clock::now() - start;
  bench->round_means_ns.push_back(static_cast<double>(Nanos(elapsed)) /
                                  static_cast<double>(lookups.size()));
  bench->reads += bench->env.TakeReads();
  bench->lookups += lookups.size();

  for (const std::string& key : lookups) {
    const auto before = Clock::now();
    Miss(&bench->db, key);
    bench->latency_ns.Add(Nanos(Clock::now() - before));
  }
}

// The cost of the clock-read pair that wraps each latency sample.
double ClockPairOverheadNs() {
  const auto start = Clock::now();
  std::uint64_t sink = 0;
  for (int sample = 0; sample < kClockSamples; ++sample) {
    sink += Nanos(Clock::now() - Clock::now());
  }
  const auto elapsed = Clock::now() - start;
  return sink == 0 ? 0 : static_cast<double>(Nanos(elapsed)) / static_cast<double>(kClockSamples);
}

double Median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

// Coefficient of variation of the per-round means: the run-to-run noise.
double CoefficientOfVariation(const std::vector<double>& values) {
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
  return mean == 0 ? 0 : std::sqrt(variance) / mean;
}

std::size_t ParseCount(const char* text, std::size_t fallback) {
  if (text == nullptr) {
    return fallback;
  }
  const unsigned long long parsed = std::strtoull(text, nullptr, 10);
  return parsed == 0 ? fallback : static_cast<std::size_t>(parsed);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <scratch-dir> [keys] [lookups] [rounds]\n", argv[0]);
    return 2;
  }
  const std::filesystem::path scratch = argv[1];
  const std::size_t keys = ParseCount(argc > 2 ? argv[2] : nullptr, kDefaultKeys);
  const std::size_t lookup_count = ParseCount(argc > 3 ? argv[3] : nullptr, kDefaultLookups);
  const std::size_t rounds = ParseCount(argc > 4 ? argv[4] : nullptr, kDefaultRounds);

  std::vector<std::string> lookups;
  lookups.reserve(lookup_count);
  std::mt19937_64 random(kSeed + 1);
  std::uniform_int_distribution<std::uint64_t> pick(0, keys - 1);
  for (std::size_t index = 0; index < lookup_count; ++index) {
    lookups.push_back(Key(2 * pick(random) + 1));
  }

  std::vector<std::unique_ptr<Bench>> benches;
  for (const auto& [name, filter, cache] :
       {std::tuple{"filter on,  cache on ", true, kBlockCacheBytes},
        std::tuple{"filter off, cache on ", false, kBlockCacheBytes},
        std::tuple{"filter on,  cache off", true, std::size_t{0}},
        std::tuple{"filter off, cache off", false, std::size_t{0}}}) {
    auto bench = std::make_unique<Bench>();
    bench->name = name;
    bench->bloom_filter = filter;
    bench->block_cache_size = cache;
    bench->path = scratch / ("negative_lookup_bench_" + std::to_string(benches.size()));
    Load(bench.get(), keys);
    benches.push_back(std::move(bench));
  }

  for (std::size_t round = 0; round < rounds; ++round) {
    for (auto& bench : benches) {
      MeasureRound(bench.get(), lookups);
    }
  }

  std::printf("keys=%zu lookups=%zu rounds=%zu value=%zuB write_buffer=%zuB clock_pair=%.0fns\n",
              keys, lookup_count, rounds, kValueSize, kWriteBuffer, ClockPairOverheadNs());
  std::printf("%-22s %7s %10s %10s %10s %12s %7s\n", "config", "tables", "mean ns", "p50 ns",
              "p99 ns", "reads/lookup", "CV");
  for (auto& bench : benches) {
    std::printf("%-22s %7zu %10.0f %10.0f %10.0f %12.2f %6.1f%%\n", bench->name,
                CountTables(bench->path), Median(bench->round_means_ns),
                bench->latency_ns.Percentile(50.0), bench->latency_ns.Percentile(99.0),
                static_cast<double>(bench->reads) / static_cast<double>(bench->lookups),
                100.0 * CoefficientOfVariation(bench->round_means_ns));
    static_cast<void>(bench->db.Close());
    std::filesystem::remove_all(bench->path);
  }
  return 0;
}

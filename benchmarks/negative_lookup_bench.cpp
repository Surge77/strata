// Phase 6 gate: the cost of Get for keys that are not in the database, with
// the per-table Bloom filter on and off, and with the block cache on and off.
//
// Workload: `keys` even-numbered keys are written in a shuffled order with a
// small write buffer, so the database ends up as many level-0 tables that
// each span the whole key range -- the case a filter exists for, because a
// missing key falls inside every table's range. Lookups are odd-numbered
// keys, so every one misses. The database is reopened before measuring.
//
// Usage: negative_lookup_bench <scratch-dir> [keys] [lookups] [runs]

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

constexpr std::size_t kWriteBuffer = 1024 * 1024;
constexpr std::size_t kValueSize = 100;
constexpr std::uint64_t kSeed = 20261004;
constexpr std::size_t kDefaultKeys = 200'000;
constexpr std::size_t kDefaultLookups = 100'000;
constexpr int kDefaultRuns = 5;
constexpr std::size_t kWarmupDivisor = 10;

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

struct Config {
  const char* name;
  bool bloom_filter;
  std::size_t block_cache_size;
};

struct RunResult {
  double mean_ns = 0;
  double p50_ns = 0;
  double p99_ns = 0;
  double reads_per_lookup = 0;
};

[[noreturn]] void Fail(const std::string& what, const strata::Status& status) {
  std::fprintf(stderr, "%s: %s\n", what.c_str(), status.ToString().c_str());
  std::exit(1);
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

void Load(const strata::Options& options, std::size_t keys) {
  std::vector<std::uint64_t> order(keys);
  for (std::size_t index = 0; index < keys; ++index) {
    order[index] = 2 * static_cast<std::uint64_t>(index);
  }
  std::mt19937_64 random(kSeed);
  std::shuffle(order.begin(), order.end(), random);
  strata::DB db;
  if (const auto status = strata::DB::Open(options, &db); !status.ok()) {
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
}

RunResult MeasureRun(strata::DB* db, CountingEnv* env, const std::vector<std::string>& lookups) {
  const std::size_t warmup = lookups.size() / kWarmupDivisor;
  for (std::size_t index = 0; index < warmup; ++index) {
    static_cast<void>(db->Get(lookups[index]));
  }
  static_cast<void>(env->TakeReads());
  strata::Histogram latency;
  const auto start = std::chrono::steady_clock::now();
  for (const std::string& key : lookups) {
    const auto before = std::chrono::steady_clock::now();
    const auto result = db->Get(key);
    const auto after = std::chrono::steady_clock::now();
    if (!result.status().IsNotFound()) {
      Fail("lookup of " + key + " should miss", result.status());
    }
    latency.Add(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
  }
  const auto total = std::chrono::steady_clock::now() - start;
  const auto lookup_count = static_cast<double>(lookups.size());
  return {
      .mean_ns =
          static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(total).count()) /
          lookup_count,
      .p50_ns = latency.Percentile(50.0),
      .p99_ns = latency.Percentile(99.0),
      .reads_per_lookup = static_cast<double>(env->TakeReads()) / lookup_count,
  };
}

double Median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

// Coefficient of variation of the per-run means: the run-to-run noise.
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
    std::fprintf(stderr, "usage: %s <scratch-dir> [keys] [lookups] [runs]\n", argv[0]);
    return 2;
  }
  const std::filesystem::path scratch = argv[1];
  const std::size_t keys = ParseCount(argc > 2 ? argv[2] : nullptr, kDefaultKeys);
  const std::size_t lookup_count = ParseCount(argc > 3 ? argv[3] : nullptr, kDefaultLookups);
  const int runs = static_cast<int>(ParseCount(argc > 4 ? argv[4] : nullptr, kDefaultRuns));

  std::vector<std::string> lookups;
  lookups.reserve(lookup_count);
  std::mt19937_64 random(kSeed + 1);
  std::uniform_int_distribution<std::uint64_t> pick(0, keys - 1);
  for (std::size_t index = 0; index < lookup_count; ++index) {
    lookups.push_back(Key(2 * pick(random) + 1));
  }

  const std::vector<Config> configs = {
      {"filter on,  cache on ", true, 8 * 1024 * 1024},
      {"filter off, cache on ", false, 8 * 1024 * 1024},
      {"filter on,  cache off", true, 0},
      {"filter off, cache off", false, 0},
  };
  std::printf("keys=%zu lookups=%zu runs=%d value=%zuB write_buffer=%zuB\n", keys, lookup_count,
              runs, kValueSize, kWriteBuffer);
  std::printf("%-22s %7s %10s %10s %10s %12s %7s\n", "config", "tables", "mean ns", "p50 ns",
              "p99 ns", "reads/lookup", "CV");
  for (const Config& config : configs) {
    const std::filesystem::path db_path = scratch / "negative_lookup_bench";
    std::filesystem::remove_all(db_path);
    CountingEnv env;
    const strata::Options options{.path = db_path.string(),
                                  .write_buffer_size = kWriteBuffer,
                                  .bloom_filter = config.bloom_filter,
                                  .block_cache_size = config.block_cache_size,
                                  .env = &env};
    Load(options, keys);
    strata::DB db;
    if (const auto status = strata::DB::Open(options, &db); !status.ok()) {
      Fail("reopen", status);
    }
    std::vector<double> means;
    RunResult last;
    for (int run = 0; run < runs; ++run) {
      last = MeasureRun(&db, &env, lookups);
      means.push_back(last.mean_ns);
    }
    std::printf("%-22s %7zu %10.0f %10.0f %10.0f %12.2f %6.1f%%\n", config.name,
                CountTables(db_path), Median(means), last.p50_ns, last.p99_ns,
                last.reads_per_lookup, 100.0 * CoefficientOfVariation(means));
    static_cast<void>(db.Close());
    std::filesystem::remove_all(db_path);
  }
  return 0;
}

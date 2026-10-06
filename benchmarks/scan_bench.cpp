// Phase 10: range-scan throughput.
//
//   forward   one full scan in key order, entries/s
//   backward  one full scan in reverse, entries/s
//   range     short bounded scans of ~100 keys from random start keys, scans/s
//
// `keys` 100-byte values are loaded in random order with default options, the
// database is reopened and its background compactions waited out, so the
// scans read a realistic multi-level tree. Each figure is the median of
// `rounds` passes over the same database, with its coefficient of variation.
//
// Usage: scan_bench <scratch-dir> [keys] [rounds]

#include "strata/write_batch.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "db/db_impl.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kDefaultKeys = 1'000'000;
constexpr int kDefaultRounds = 3;
constexpr std::size_t kValueSize = 100;
constexpr int kRangeScans = 20'000;
constexpr std::uint64_t kRangeWidth = 100;
constexpr std::uint64_t kSeed = 20261010;

std::string Key(std::uint64_t number) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "key%016llu", static_cast<unsigned long long>(number));
  return buffer;
}

[[noreturn]] void Fail(const char* what, const strata::Status& status) {
  std::fprintf(stderr, "%s: %s\n", what, status.ToString().c_str());
  std::exit(1);
}

std::unique_ptr<strata::DBImpl> Open(const std::filesystem::path& path) {
  auto opened = strata::DBImpl::Open({.path = path.string()});
  if (!opened.ok()) {
    Fail("open", opened.status());
  }
  return std::move(*opened);
}

double FullScan(strata::DBImpl* db, bool forward, std::size_t expected) {
  auto it = db->NewIterator({});
  std::size_t entries = 0;
  const auto start = Clock::now();
  if (forward) {
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ++entries;
    }
  } else {
    for (it->SeekToLast(); it->Valid(); it->Prev()) {
      ++entries;
    }
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  if (!it->status().ok() || entries != expected) {
    Fail("full scan", it->status().ok() ? strata::Status::Corruption("wrong count") : it->status());
  }
  return static_cast<double>(entries) / seconds;
}

double RangeScans(strata::DBImpl* db, std::size_t keys, std::mt19937_64* random) {
  std::uniform_int_distribution<std::uint64_t> pick(0, keys - kRangeWidth - 1);
  const auto start = Clock::now();
  for (int scan = 0; scan < kRangeScans; ++scan) {
    const std::uint64_t first = pick(*random);
    auto it = db->NewIterator({.lower_bound = Key(first), .upper_bound = Key(first + kRangeWidth)});
    std::uint64_t entries = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ++entries;
    }
    if (entries != kRangeWidth) {
      Fail("range scan", strata::Status::Corruption("wrong count"));
    }
  }
  return kRangeScans / std::chrono::duration<double>(Clock::now() - start).count();
}

struct Summary {
  double median = 0;
  double cv = 0;
};

Summary Summarise(std::vector<double> values) {
  double mean =
      std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  double variance = 0;
  for (const double value : values) {
    variance += (value - mean) * (value - mean);
  }
  variance /= static_cast<double>(values.size());
  std::sort(values.begin(), values.end());
  return {.median = values[values.size() / 2], .cv = mean == 0 ? 0 : std::sqrt(variance) / mean};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <scratch-dir> [keys] [rounds]\n", argv[0]);
    return 2;
  }
  const std::filesystem::path path = std::filesystem::path(argv[1]) / "scan_bench";
  const std::size_t keys = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : kDefaultKeys;
  const int rounds = argc > 3 ? std::max(1, std::atoi(argv[3])) : kDefaultRounds;

  std::filesystem::remove_all(path);
  {
    auto db = Open(path);
    std::vector<std::uint64_t> order(keys);
    std::iota(order.begin(), order.end(), std::uint64_t{0});
    std::mt19937_64 random(kSeed);
    std::shuffle(order.begin(), order.end(), random);
    const std::string value(kValueSize, 'v');
    for (const std::uint64_t number : order) {
      strata::WriteBatch batch;
      batch.Put(Key(number), value);
      if (const auto s = db->Write({}, batch); !s.ok()) {
        Fail("load", s);
      }
    }
    db->WaitForCompactionsForTest();
    if (const auto s = db->Close(); !s.ok()) {
      Fail("close", s);
    }
  }
  auto db = Open(path);
  db->WaitForCompactionsForTest();
  std::printf("keys=%zu value=%zuB rounds=%d tables per level:", keys, kValueSize, rounds);
  for (int level = 0; level < strata::kNumLevels; ++level) {
    std::printf(" %zu", db->FilesAtLevelForTest(level).size());
  }
  std::printf("\n");

  std::vector<double> forward;
  std::vector<double> backward;
  std::vector<double> ranges;
  std::mt19937_64 random(kSeed + 1);
  for (int round = 0; round < rounds; ++round) {
    forward.push_back(FullScan(db.get(), /*forward=*/true, keys));
    backward.push_back(FullScan(db.get(), /*forward=*/false, keys));
    ranges.push_back(RangeScans(db.get(), keys, &random));
  }
  const Summary f = Summarise(forward);
  const Summary b = Summarise(backward);
  const Summary r = Summarise(ranges);
  std::printf("%-26s %14.0f  CV %5.1f%%\n", "forward entries/s", f.median, 100 * f.cv);
  std::printf("%-26s %14.0f  CV %5.1f%%\n", "backward entries/s", b.median, 100 * b.cv);
  std::printf("%-26s %14.0f  CV %5.1f%%\n", "100-key range scans/s", r.median, 100 * r.cv);
  static_cast<void>(db->Close());
  std::filesystem::remove_all(path);
  return 0;
}

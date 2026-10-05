// Phase 7 gate: write amplification -- bytes written to SSTables (by flush
// and by compaction) per byte of user data -- for three workloads:
//
//   fillseq    keys written in increasing order
//   fillrandom each key written once, in random order
//   overwrite  keys drawn at random from a key space a quarter of the writes,
//              so most writes replace an existing key
//
// Each workload runs on a fresh database with default options, then flushes
// the last memtable and waits for background compaction to finish before
// reporting.
//
// WA = table bytes (flush + compaction) / user payload bytes (key + value).
// It excludes the write-ahead log (about one more byte per payload byte) and
// MANIFEST records. Tables prefix-compress keys, so a sequential load can come
// in under 1. Writes/s includes any time writes spent stalled.
//
// Usage: write_amp_bench <scratch-dir> [writes]

#include "strata/write_batch.hpp"

#include <algorithm>
#include <chrono>
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

constexpr std::size_t kDefaultWrites = 1'000'000;
constexpr std::size_t kValueSize = 100;
constexpr std::uint64_t kSeed = 20261006;
constexpr std::uint64_t kOverwriteKeySpaceDivisor = 4;
constexpr double kBytesPerMiB = 1024.0 * 1024.0;

std::string Key(std::uint64_t number) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "key%016llu", static_cast<unsigned long long>(number));
  return buffer;
}

[[noreturn]] void Fail(const char* what, const strata::Status& status) {
  std::fprintf(stderr, "%s: %s\n", what, status.ToString().c_str());
  std::exit(1);
}

std::vector<std::uint64_t> KeyOrder(const std::string& workload, std::size_t writes) {
  std::vector<std::uint64_t> order(writes);
  std::iota(order.begin(), order.end(), std::uint64_t{0});
  std::mt19937_64 random(kSeed);
  if (workload == "fillrandom" || writes < kOverwriteKeySpaceDivisor) {
    std::shuffle(order.begin(), order.end(), random);
  } else if (workload == "overwrite") {
    std::uniform_int_distribution<std::uint64_t> pick(0, writes / kOverwriteKeySpaceDivisor - 1);
    for (auto& key : order) {
      key = pick(random);
    }
  }
  return order;
}

void Run(const std::filesystem::path& scratch, const std::string& workload, std::size_t writes) {
  const std::filesystem::path path = scratch / ("write_amp_" + workload);
  std::filesystem::remove_all(path);
  auto opened = strata::DBImpl::Open({.path = path.string()});
  if (!opened.ok()) {
    Fail("open", opened.status());
  }
  auto db = std::move(*opened);
  const std::string value(kValueSize, 'v');
  const auto order = KeyOrder(workload, writes);

  const auto start = std::chrono::steady_clock::now();
  for (const std::uint64_t number : order) {
    strata::WriteBatch batch;
    batch.Put(Key(number), value);
    if (const auto status = db->Write({}, batch); !status.ok()) {
      Fail("write", status);
    }
  }
  const auto written = std::chrono::steady_clock::now();
  // Flush the last memtable too, so every user byte is counted in a table.
  if (const auto status = db->FlushMemTableForTest(); !status.ok()) {
    Fail("flush", status);
  }
  db->WaitForCompactionsForTest();
  const auto settled = std::chrono::steady_clock::now();

  const strata::DBStats stats = db->GetStats();
  const double user = static_cast<double>(writes) * static_cast<double>(Key(0).size() + kValueSize);
  const double tables =
      static_cast<double>(stats.flush_bytes_written + stats.compaction_bytes_written);
  const double write_seconds = std::chrono::duration<double>(written - start).count();
  const double settle_seconds = std::chrono::duration<double>(settled - written).count();
  std::printf("%-11s %11.1f %9.1f %9.1f %7.2f %9.0f %7.1f %6llu %6llu %6llu  ", workload.c_str(),
              user / kBytesPerMiB, static_cast<double>(stats.flush_bytes_written) / kBytesPerMiB,
              static_cast<double>(stats.compaction_bytes_written) / kBytesPerMiB, tables / user,
              static_cast<double>(writes) / write_seconds, settle_seconds,
              static_cast<unsigned long long>(stats.compactions),
              static_cast<unsigned long long>(stats.trivial_moves),
              static_cast<unsigned long long>(stats.write_slowdowns + stats.write_stops));
  for (int level = 0; level < strata::kNumLevels; ++level) {
    std::printf("%zu ", db->FilesAtLevelForTest(level).size());
  }
  std::printf("\n");
  if (const auto status = db->Close(); !status.ok()) {
    Fail("close", status);
  }
  std::filesystem::remove_all(path);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <scratch-dir> [writes]\n", argv[0]);
    return 2;
  }
  const std::filesystem::path scratch = argv[1];
  const unsigned long long parsed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0;
  const std::size_t writes = parsed == 0 ? kDefaultWrites : static_cast<std::size_t>(parsed);

  std::printf("writes=%zu value=%zuB key=19B default options\n", writes, kValueSize);
  std::printf("%-11s %11s %9s %9s %7s %9s %7s %6s %6s %6s  %s\n", "workload", "payload MiB",
              "flush MiB", "comp MiB", "WA", "writes/s", "settle", "comps", "moves", "stalls",
              "tables per level");
  for (const char* workload : {"fillseq", "fillrandom", "overwrite"}) {
    Run(scratch, workload, writes);
  }
  return 0;
}

#include "stratabench/workloads.hpp"

#include "strata/iterator.hpp"
#include "strata/write_batch.hpp"

#include <algorithm>
#include <chrono>
#include <latch>
#include <numeric>
#include <thread>

#include "util/crc32c.hpp"

namespace strata::bench {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kSyncDivisor = 1000;  // fillsync writes num / 1000 keys
constexpr std::uint64_t kMinSyncWrites = 100;
constexpr std::uint64_t kScanLength = 100;  // entries read after each seek
constexpr double kYcsbAReadFraction = 0.5;
constexpr double kYcsbBReadFraction = 0.95;
// A timed round checks its deadline once per chunk, not once per operation.
constexpr std::uint64_t kChunkOps = 256;
constexpr std::size_t kCalibrationBytes = 256 * 1024;
constexpr double kBytesPerMegabyte = 1e6;
// Spreads per-round, per-thread seeds far apart.
constexpr std::uint64_t kRoundSeedStride = 1'000'003;
constexpr std::uint64_t kThreadSeedStride = 7'919;
constexpr std::uint64_t kGoldenGamma = 0x9E3779B97F4A7C15ULL;
// MixedOp indexes: each thread of each round gets its own 2^40-wide range.
constexpr unsigned kThreadIndexShift = 40;
constexpr unsigned kRoundIndexShift = 48;

std::uint64_t SeedFor(const BenchConfig& config, int round, int thread) {
  return config.seed + static_cast<std::uint64_t>(round) * kRoundSeedStride +
         static_cast<std::uint64_t>(thread) * kThreadSeedStride;
}

void PutOrFail(DBImpl* db, Slice key, Slice value, bool sync) {
  WriteBatch batch;  // what DB::Put does, so the timed path is the public one
  batch.Put(key, value);
  if (const Status status = db->Write({.sync = sync}, batch); !status.ok()) {
    Fail("put", status);
  }
}

double Fill(const BenchConfig& config, bool random_keys, bool sync, int round) {
  const std::uint64_t total =
      sync ? std::max(config.num / kSyncDivisor, kMinSyncWrites) : config.num;
  auto db = OpenFresh(config.scratch / "fill");
  const ValueSource values(config.value_size, config.seed);
  const double seconds = TimeThreads(config.threads, [&](int thread) {
    const Share share = ShareOf(total, thread, config.threads);
    KeyChooser chooser(KeyDistribution::kUniform, config.num, SeedFor(config, round, thread));
    KeyBuffer key;
    for (std::uint64_t index = share.begin; index < share.end; ++index) {
      const std::uint64_t number = random_keys ? chooser.Next() : index;
      PutOrFail(db.get(), FormatKey(number, &key), values.At(index), sync);
    }
  });
  CloseOrFail(db.get());
  return static_cast<double>(total) / seconds;
}

double Reads(const BenchConfig& config, LoadedDb* loaded, KeyDistribution distribution,
             bool missing, int round) {
  loaded->PrepareRound(config.cold, /*writes=*/false);
  DBImpl* db = loaded->db();
  const Timed timed = RunForDuration(config.threads, config.min_round, [&](int thread) {
    return [db, missing,
            chooser = KeyChooser(distribution, loaded->keys(), SeedFor(config, round, thread))](
               std::uint64_t /*first*/, std::uint64_t count) mutable {
      KeyBuffer key;
      for (std::uint64_t op = 0; op < count; ++op) {
        const std::uint64_t number = chooser.Next();
        if (missing) {
          const auto r = db->Get({}, FormatMissingKey(number, &key));
          if (!r.status().IsNotFound()) {
            Fail("get of a missing key", r.ok() ? Status::Corruption("found") : r.status());
          }
        } else if (const auto r = db->Get({}, FormatKey(number, &key)); !r.ok()) {
          Fail("get", r.status());
        }
      }
    };
  });
  return static_cast<double>(timed.operations) / timed.seconds;
}

// One full forward scan; fails unless it sees exactly every key.
std::uint64_t ScanOnce(LoadedDb* loaded) {
  auto it = loaded->db()->NewIterator({});
  std::uint64_t entries = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ++entries;
  }
  if (!it->status().ok() || entries != loaded->keys()) {
    Fail("scanfull", it->status().ok() ? Status::Corruption("wrong entry count") : it->status());
  }
  return entries;
}

double ScanFull(const BenchConfig& config, LoadedDb* loaded) {
  loaded->PrepareRound(config.cold, /*writes=*/false);
  std::uint64_t entries = 0;
  const double seconds = TimeThreads(1, [&](int) {
    const Clock::time_point deadline = Clock::now() + config.min_round;
    do {
      entries += ScanOnce(loaded);
    } while (Clock::now() < deadline);
  });
  return static_cast<double>(entries) / seconds;
}

double SeekRandom(const BenchConfig& config, LoadedDb* loaded, int round) {
  loaded->PrepareRound(config.cold, /*writes=*/false);
  DBImpl* db = loaded->db();
  const Timed timed = RunForDuration(config.threads, config.min_round, [&](int thread) {
    return [db, chooser = KeyChooser(KeyDistribution::kUniform, loaded->keys(),
                                     SeedFor(config, round, thread))](std::uint64_t /*first*/,
                                                                      std::uint64_t count) mutable {
      KeyBuffer key;
      for (std::uint64_t scan = 0; scan < count; ++scan) {
        auto it = db->NewIterator({});
        std::uint64_t entries = 0;
        for (it->Seek(FormatKey(chooser.Next(), &key)); it->Valid() && entries < kScanLength;
             it->Next()) {
          ++entries;
        }
        if (!it->status().ok() || entries == 0) {
          Fail("seekrandom", it->status().ok() ? Status::Corruption("empty scan") : it->status());
        }
      }
    };
  });
  return static_cast<double>(timed.operations) / timed.seconds;
}

// Fixed CPU work with no database code and no I/O: crc32c over a buffer that
// fits in L2. A noisy figure here means the host, not the engine, is moving
// the numbers.
double CpuCalibration(const BenchConfig& config) {
  const ValueSource source(kCalibrationBytes, config.seed);
  const Slice buffer = source.At(0);
  std::uint64_t bytes = 0;
  std::uint32_t crc = 0;
  const double seconds = TimeThreads(1, [&](int) {
    const Clock::time_point deadline = Clock::now() + config.min_round;
    do {
      crc = crc32c::Extend(crc, buffer);
      bytes += buffer.size();
    } while (Clock::now() < deadline);
  });
  if (crc == 0 && bytes == 0) {
    Fail("cpu", Status::Internal("no work done"));  // also keeps `crc` observable
  }
  return static_cast<double>(bytes) / seconds / kBytesPerMegabyte;
}

}  // namespace

double MixedThroughput(const BenchConfig& config, LoadedDb* loaded, double read_fraction,
                       int threads, int round) {
  loaded->PrepareRound(config.cold, /*writes=*/read_fraction < 1.0);
  const MixedOp op(loaded->db(), loaded->keys(), read_fraction, config.value_size,
                   SeedFor(config, round, 0));
  const Timed timed = RunForDuration(threads, config.min_round, [&](int thread) {
    // Distinct threads and rounds use distinct operation indexes, so they differ.
    const std::uint64_t base = (static_cast<std::uint64_t>(round) << kRoundIndexShift) +
                               (static_cast<std::uint64_t>(thread) << kThreadIndexShift);
    return [&op, base](std::uint64_t first, std::uint64_t count) {
      for (std::uint64_t index = first; index < first + count; ++index) {
        op(base + index);
      }
    };
  });
  return static_cast<double>(timed.operations) / timed.seconds;
}

const std::vector<std::string_view>& AllWorkloads() {
  static const std::vector<std::string_view> kAll = {
      "cpu",         "fillseq",  "fillrandom", "fillsync", "readrandom", "readzipf",
      "readmissing", "scanfull", "seekrandom", "ycsb-a",   "ycsb-b"};
  return kAll;
}

bool IsKnownWorkload(std::string_view name) {
  const auto& all = AllWorkloads();
  return std::find(all.begin(), all.end(), name) != all.end();
}

bool NeedsLoadedDb(std::string_view name) {
  return IsKnownWorkload(name) && name != "cpu" && !name.starts_with("fill");
}

std::string_view UnitOf(std::string_view name) {
  if (name == "cpu") {
    return "MB/s";
  }
  if (name == "scanfull") {
    return "entries/s";
  }
  return name == "seekrandom" ? "scans/s" : "ops/s";
}

double RunWorkloadRound(std::string_view name, const BenchConfig& config, LoadedDb* loaded,
                        int round) {
  using enum KeyDistribution;
  if (name == "cpu") return CpuCalibration(config);
  if (name == "fillseq") return Fill(config, /*random_keys=*/false, /*sync=*/false, round);
  if (name == "fillrandom") return Fill(config, /*random_keys=*/true, /*sync=*/false, round);
  if (name == "fillsync") return Fill(config, /*random_keys=*/true, /*sync=*/true, round);
  if (name == "readrandom") return Reads(config, loaded, kUniform, /*missing=*/false, round);
  if (name == "readzipf") return Reads(config, loaded, kZipfian, /*missing=*/false, round);
  if (name == "readmissing") return Reads(config, loaded, kUniform, /*missing=*/true, round);
  if (name == "scanfull") return ScanFull(config, loaded);
  if (name == "seekrandom") return SeekRandom(config, loaded, round);
  if (name == "ycsb-a") {
    return MixedThroughput(config, loaded, kYcsbAReadFraction, config.threads, round);
  }
  if (name == "ycsb-b") {
    return MixedThroughput(config, loaded, kYcsbBReadFraction, config.threads, round);
  }
  Fail("workload", Status::InvalidArgument(name));
}

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

MixedOp::MixedOp(DBImpl* db, std::uint64_t keys, double read_fraction, std::size_t value_size,
                 std::uint64_t seed)
    : db_(db),
      keys_(keys),
      read_fraction_(read_fraction),
      seed_(seed),
      zipf_(keys, ZipfianGenerator::kDefaultTheta, seed),
      values_(value_size, seed) {}

void MixedOp::operator()(std::uint64_t index) const {
  Random random(seed_ ^ (index * kGoldenGamma));
  const bool is_read = UniformDouble(&random) < read_fraction_;
  const std::uint64_t number =
      keys_ <= 1 ? 0 : ZipfianGenerator::Scramble(zipf_.RankFor(UniformDouble(&random)), keys_);
  KeyBuffer key;
  if (is_read) {
    if (const auto r = db_->Get({}, FormatKey(number, &key)); !r.ok()) {
      Fail("get", r.status());
    }
  } else {
    PutOrFail(db_, FormatKey(number, &key), values_.At(index), /*sync=*/false);
  }
}

}  // namespace strata::bench

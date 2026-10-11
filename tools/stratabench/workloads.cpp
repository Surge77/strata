#include "stratabench/workloads.hpp"

#include "strata/iterator.hpp"
#include "strata/write_batch.hpp"

#include <algorithm>
#include <chrono>
#include <latch>
#include <thread>

namespace strata::bench {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kSyncDivisor = 1000;  // fillsync writes num / 1000 keys
constexpr std::uint64_t kMinSyncWrites = 100;
constexpr std::uint64_t kScanLength = 100;  // entries read after each seek
constexpr double kYcsbAReadFraction = 0.5;
constexpr double kYcsbBReadFraction = 0.95;
// Spreads per-round, per-thread seeds far apart.
constexpr std::uint64_t kRoundSeedStride = 1'000'003;
constexpr std::uint64_t kThreadSeedStride = 7'919;
constexpr std::uint64_t kGoldenGamma = 0x9E3779B97F4A7C15ULL;

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
  loaded->PrepareRound(config.cold);
  DBImpl* db = loaded->db();
  const std::uint64_t total = config.reads_per_round();
  const double seconds = TimeThreads(config.threads, [&](int thread) {
    const Share share = ShareOf(total, thread, config.threads);
    KeyChooser chooser(distribution, loaded->keys(), SeedFor(config, round, thread));
    KeyBuffer key;
    for (std::uint64_t index = share.begin; index < share.end; ++index) {
      const std::uint64_t number = chooser.Next();
      if (missing) {
        if (const auto r = db->Get({}, FormatMissingKey(number, &key)); !r.status().IsNotFound()) {
          Fail("get of a missing key", r.ok() ? Status::Corruption("found") : r.status());
        }
      } else if (const auto r = db->Get({}, FormatKey(number, &key)); !r.ok()) {
        Fail("get", r.status());
      }
    }
  });
  return static_cast<double>(total) / seconds;
}

double ScanFull(const BenchConfig& config, LoadedDb* loaded) {
  loaded->PrepareRound(config.cold);
  std::uint64_t entries = 0;
  std::unique_ptr<Iterator> it;
  const double seconds = TimeThreads(1, [&](int) {
    it = loaded->db()->NewIterator({});
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ++entries;
    }
  });
  if (!it->status().ok() || entries != loaded->keys()) {
    Fail("scanfull", it->status().ok() ? Status::Corruption("wrong entry count") : it->status());
  }
  return static_cast<double>(entries) / seconds;
}

double SeekRandom(const BenchConfig& config, LoadedDb* loaded, int round) {
  loaded->PrepareRound(config.cold);
  DBImpl* db = loaded->db();
  const std::uint64_t scans = std::max<std::uint64_t>(config.reads_per_round() / kScanLength, 1);
  const double seconds = TimeThreads(config.threads, [&](int thread) {
    const Share share = ShareOf(scans, thread, config.threads);
    KeyChooser chooser(KeyDistribution::kUniform, loaded->keys(), SeedFor(config, round, thread));
    KeyBuffer key;
    for (std::uint64_t scan = share.begin; scan < share.end; ++scan) {
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
  });
  return static_cast<double>(scans) / seconds;
}

double Mixed(const BenchConfig& config, LoadedDb* loaded, double read_fraction, int round) {
  loaded->PrepareRound(config.cold);
  const std::uint64_t total = config.reads_per_round();
  const MixedOp op(loaded->db(), loaded->keys(), read_fraction, config.value_size,
                   SeedFor(config, round, 0));
  // Distinct rounds use distinct operation indexes, so they differ.
  const std::uint64_t base = static_cast<std::uint64_t>(round) * total;
  const double seconds = TimeThreads(config.threads, [&](int thread) {
    const Share share = ShareOf(total, thread, config.threads);
    for (std::uint64_t index = share.begin; index < share.end; ++index) {
      op(base + index);
    }
  });
  return static_cast<double>(total) / seconds;
}

}  // namespace

const std::vector<std::string_view>& AllWorkloads() {
  static const std::vector<std::string_view> kAll = {
      "fillseq",     "fillrandom", "fillsync",   "readrandom", "readzipf",
      "readmissing", "scanfull",   "seekrandom", "ycsb-a",     "ycsb-b"};
  return kAll;
}

bool IsKnownWorkload(std::string_view name) {
  const auto& all = AllWorkloads();
  return std::find(all.begin(), all.end(), name) != all.end();
}

bool NeedsLoadedDb(std::string_view name) {
  return IsKnownWorkload(name) && !name.starts_with("fill");
}

std::string_view UnitOf(std::string_view name) {
  if (name == "scanfull") {
    return "entries/s";
  }
  return name == "seekrandom" ? "scans/s" : "ops/s";
}

double RunWorkloadRound(std::string_view name, const BenchConfig& config, LoadedDb* loaded,
                        int round) {
  if (name == "fillseq") return Fill(config, /*random_keys=*/false, /*sync=*/false, round);
  if (name == "fillrandom") return Fill(config, /*random_keys=*/true, /*sync=*/false, round);
  if (name == "fillsync") return Fill(config, /*random_keys=*/true, /*sync=*/true, round);
  if (name == "readrandom") return Reads(config, loaded, KeyDistribution::kUniform, false, round);
  if (name == "readzipf") return Reads(config, loaded, KeyDistribution::kZipfian, false, round);
  if (name == "readmissing") return Reads(config, loaded, KeyDistribution::kUniform, true, round);
  if (name == "scanfull") return ScanFull(config, loaded);
  if (name == "seekrandom") return SeekRandom(config, loaded, round);
  if (name == "ycsb-a") return Mixed(config, loaded, kYcsbAReadFraction, round);
  if (name == "ycsb-b") return Mixed(config, loaded, kYcsbBReadFraction, round);
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

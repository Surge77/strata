#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "stratabench/bench_db.hpp"
#include "stratabench/key_generator.hpp"
#include "stratabench/stats.hpp"

namespace strata::bench {

struct BenchConfig {
  std::filesystem::path scratch;  // databases are created under here
  std::uint64_t num = 1'000'000;  // keys
  std::size_t value_size = 100;
  int threads = 1;
  std::uint64_t reads = 0;  // operations per read round; 0 means `num`
  bool cold = false;
  Protocol protocol;
  std::uint64_t seed = 20261011;

  [[nodiscard]] std::uint64_t reads_per_round() const noexcept { return reads == 0 ? num : reads; }
};

struct WorkloadResult {
  std::string name;
  std::string unit;            // "ops/s", "entries/s", "scans/s"
  std::vector<double> values;  // one per measured round
  Summary summary;
};

// Every closed-loop workload, in the order a full run executes them: fresh-
// database writes, then reads of the shared loaded database, then the YCSB
// mixes, whose Puts change that database (docs/benchmarks.md).
[[nodiscard]] const std::vector<std::string_view>& AllWorkloads();
[[nodiscard]] bool IsKnownWorkload(std::string_view name);
[[nodiscard]] bool NeedsLoadedDb(std::string_view name);
[[nodiscard]] std::string_view UnitOf(std::string_view name);

// One measured round of `name`, returning its figure. `loaded` may be null
// when NeedsLoadedDb(name) is false.
[[nodiscard]] double RunWorkloadRound(std::string_view name, const BenchConfig& config,
                                      LoadedDb* loaded, int round);

// Runs `body(thread)` on `threads` threads released together; returns the
// seconds from release until the last one finished.
[[nodiscard]] double TimeThreads(int threads, const std::function<void(int)>& body);

// The [begin, end) slice of `total` operations that `thread` of `threads` does.
struct Share {
  std::uint64_t begin;
  std::uint64_t end;
};
[[nodiscard]] Share ShareOf(std::uint64_t total, int thread, int threads) noexcept;

// One operation of a YCSB-style mix on a loaded database of `keys` keys: a Get
// of an existing key with probability `read_fraction`, otherwise a Put
// overwriting one; keys are scrambled-Zipfian. Operation `index` is a pure
// function of (seed, index), so one MixedOp can be called from many threads
// at once and a schedule replays identically. Used by the closed-loop mixes
// and by the open-loop harness.
class MixedOp {
 public:
  MixedOp(DBImpl* db, std::uint64_t keys, double read_fraction, std::size_t value_size,
          std::uint64_t seed);
  void operator()(std::uint64_t index) const;

 private:
  DBImpl* db_;
  std::uint64_t keys_;
  double read_fraction_;
  std::uint64_t seed_;
  ZipfianGenerator zipf_;
  ValueSource values_;
};

}  // namespace strata::bench

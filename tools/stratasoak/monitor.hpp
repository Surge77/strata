#pragma once

#include "strata/db.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace strata::soak {

// One look at the process and the engine during a soak.
struct ResourceSample {
  double seconds = 0;                 // since the soak started
  std::uint64_t rss_bytes = 0;        // resident memory (/proc/self/statm)
  std::uint64_t open_fds = 0;         // entries in /proc/self/fd
  std::uint64_t memtable_bytes = 0;   // active memtable
  std::uint64_t immutable_bytes = 0;  // memtable being flushed
  std::uint64_t table_files = 0;      // SSTables across all levels
};

// Samples this process; the engine figures are 0 if the database is closed.
[[nodiscard]] ResourceSample Sample(const DB& db, double seconds);

// One release-gate check over the samples: what was measured, the bound it
// was held to, and whether it held.
struct Check {
  std::string name;
  std::string measured;
  std::string bound;
  bool passed = false;
};

struct Limits {
  std::size_t write_buffer_size = 0;
};

// The soak's verdict on growth. The first quarter of the run is warm-up
// (caches fill, the tree reaches its shape for the bounded key space); after
// that, nothing may keep growing:
//  - memory: the largest RSS in the last quarter within 10% (plus 16 MiB of
//    allocator slack) of the largest in the second quarter;
//  - descriptors: the most open in the last quarter at most 16 above the most
//    in the second quarter;
//  - memtables: never above 2x write_buffer_size, active or immutable.
// Needs at least 8 samples; fewer is a failed check, not a pass.
[[nodiscard]] std::vector<Check> CheckGrowth(const std::vector<ResourceSample>& samples,
                                             const Limits& limits);

// The soak report as JSON: the run, the totals, every check, and every sample.
struct SoakSummary {
  double seconds = 0;
  int reopens = 0;
  std::uint64_t writes = 0, deletes = 0, reads = 0, scans = 0, snapshots = 0, verified = 0;
  std::string error;  // empty if every operation and check of data passed
};

[[nodiscard]] bool WriteReport(const std::filesystem::path& path, const SoakSummary& summary,
                               const std::vector<Check>& checks,
                               const std::vector<ResourceSample>& samples);

}  // namespace strata::soak

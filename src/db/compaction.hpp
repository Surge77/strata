#pragma once

#include "strata/env.hpp"
#include "strata/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "db/dbformat.hpp"
#include "db/table_cache.hpp"
#include "db/version_edit.hpp"
#include "db/version_set.hpp"

namespace strata {

// Level 0 is compacted once it holds this many tables. Writes are slowed at
// the slowdown count and stopped at the stop count until compaction catches
// up, so level 0 -- which every lookup searches table by table -- stays short.
inline constexpr std::size_t kL0CompactionTrigger = 4;
inline constexpr std::size_t kL0SlowdownWritesTrigger = 8;
inline constexpr std::size_t kL0StopWritesTrigger = 12;

// One compaction: tables from `level` merged with the tables they overlap in
// level + 1, written to level + 1.
struct Compaction {
  int level = 0;
  std::array<std::vector<FileMetaData>, 2> inputs;

  [[nodiscard]] int output_level() const noexcept { return level + 1; }

  // One table with nothing under it can simply be relinked one level down.
  [[nodiscard]] bool IsTrivialMove() const noexcept {
    return inputs[0].size() == 1 && inputs[1].empty();
  }
};

// Bytes level >= 1 may hold: `base` for level 1, ten times more per level.
[[nodiscard]] std::uint64_t MaxBytesForLevel(int level, std::uint64_t base);

// How urgently `level` needs compacting; 1 or more means it does.
[[nodiscard]] double CompactionScore(const Version& version, int level, std::uint64_t base);

// The most urgent automatic compaction, or nothing. `next_start` remembers,
// per level, the user key after which the next pick in that level begins, so
// successive compactions cycle through the whole key range.
[[nodiscard]] std::optional<Compaction> PickCompaction(
    const Version& version, const InternalKeyComparator& comparator, std::uint64_t base,
    std::array<std::string, kNumLevels>* next_start);

// Every table in `level` and what overlaps them below. Nothing if the level is
// empty or is the last one.
[[nodiscard]] std::optional<Compaction> PickLevelCompaction(const Version& version,
                                                            const InternalKeyComparator& comparator,
                                                            int level);

struct CompactionStats {
  std::uint64_t bytes_read = 0;
  std::uint64_t bytes_written = 0;
};

struct CompactionJobOptions {
  std::string dbname;
  Env* env = nullptr;
  const InternalKeyComparator* comparator = nullptr;
  TableCache* tables = nullptr;
  bool use_filter = true;
  std::size_t max_file_size = 0;
  // No snapshot can see anything older than this. With no snapshots, the
  // newest sequence number.
  SequenceNumber smallest_snapshot = 0;
  // Allocates a table number that obsolete-file cleanup will not touch.
  // Called without the DB mutex held.
  std::function<std::uint64_t()> new_file_number;
};

// Merges the compaction's inputs into new tables for its output level,
// dropping what no reader can see any more:
// - a version of a key when a newer version is visible to every snapshot;
// - a tombstone visible to every snapshot when no deeper level can hold an
//   older value for its key (otherwise that value would come back).
// All versions of one user key go to the same output table.
//
// On success the outputs are synced, their directory entry is synced, and each
// has been reopened as a check. On failure every output written is deleted
// and the inputs are untouched. `version` is the one the inputs came from.
Status RunCompaction(const CompactionJobOptions& options, const Compaction& compaction,
                     const Version& version, std::vector<FileMetaData>* outputs,
                     CompactionStats* stats);

}  // namespace strata

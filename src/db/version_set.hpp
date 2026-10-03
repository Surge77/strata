#pragma once

#include "strata/env.hpp"
#include "strata/status.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "db/dbformat.hpp"
#include "db/memtable.hpp"
#include "db/table_cache.hpp"
#include "db/version_edit.hpp"
#include "log/log_writer.hpp"

namespace strata {

// One immutable snapshot of which SSTables make up the database.
//
// Readers hold a Version (via shared_ptr) for the length of a read, so the
// files it lists cannot be deleted underneath them; garbage collection only
// removes files no live Version mentions.
class Version {
 public:
  explicit Version(const InternalKeyComparator* comparator) : comparator_(comparator) {}

  [[nodiscard]] const std::vector<FileMetaData>& files(int level) const {
    return files_[static_cast<std::size_t>(level)];
  }

  // Finds the newest version of key.user_key() at or below its sequence
  // number in this Version's tables, with the same three outcomes as the
  // memtable. Level-0 tables may overlap, so they are searched newest first;
  // deeper levels (phase 7) hold disjoint ranges.
  Result<MemTable::LookupResult> Get(const LookupKey& key, std::string* value,
                                     TableCache* tables) const;

 private:
  friend class VersionSet;

  const InternalKeyComparator* comparator_;
  std::array<std::vector<FileMetaData>, kNumLevels> files_;
};

// The MANIFEST, and the counters that must survive restarts: next file
// number, last sequence, and the number of the oldest log still needed.
//
// Not internally synchronised: the DB calls it with its mutex held.
class VersionSet {
 public:
  VersionSet(std::string dbname, Env* env, const InternalKeyComparator* comparator);

  // Loads the state named by CURRENT. Sets *existing to false (and leaves a
  // fresh, empty state) when there is no CURRENT.
  Status Recover(bool* existing);

  // Applies `edit` to the current Version and commits it: the edit is
  // appended to the MANIFEST and fdatasynced (the commit point), and only
  // then does the new Version become current. The first call after Recover
  // starts a new MANIFEST -- a snapshot of the whole state -- and publishes it
  // through CURRENT (temp file, sync, rename, SyncDir). On failure nothing
  // changes and the error is returned.
  Status LogAndApply(VersionEdit* edit);

  [[nodiscard]] std::shared_ptr<const Version> current() const { return current_; }

  std::uint64_t NewFileNumber() { return next_file_number_++; }
  void MarkFileNumberUsed(std::uint64_t number) {
    if (next_file_number_ <= number) {
      next_file_number_ = number + 1;
    }
  }

  [[nodiscard]] std::uint64_t log_number() const noexcept { return log_number_; }
  [[nodiscard]] std::uint64_t manifest_file_number() const noexcept { return manifest_number_; }
  [[nodiscard]] SequenceNumber last_sequence() const noexcept { return last_sequence_; }
  void SetLastSequence(SequenceNumber sequence) { last_sequence_ = sequence; }

  // Adds every file referenced by any Version still alive (current, or held
  // by a reader) to *live.
  void AddLiveFiles(std::set<std::uint64_t>* live);

 private:
  Status ApplyTo(const Version& base, const VersionEdit& edit, Version* result) const;
  Status WriteSnapshot(log::Writer* writer) const;
  Status SetCurrentFile(std::uint64_t manifest_number, bool* did_rename);

  const std::string dbname_;
  Env* const env_;
  const InternalKeyComparator* const comparator_;

  std::uint64_t next_file_number_ = 2;
  std::uint64_t manifest_number_ = 0;
  std::uint64_t log_number_ = 0;
  SequenceNumber last_sequence_ = 0;

  std::shared_ptr<const Version> current_;
  std::vector<std::weak_ptr<const Version>> versions_;

  std::unique_ptr<WritableFile> manifest_file_;  // outlives manifest_log_
  std::unique_ptr<log::Writer> manifest_log_;
};

}  // namespace strata

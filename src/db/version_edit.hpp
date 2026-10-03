#pragma once

#include "strata/slice.hpp"
#include "strata/status.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "db/dbformat.hpp"

namespace strata {

inline constexpr int kNumLevels = 7;

// One SSTable as the MANIFEST describes it.
struct FileMetaData {
  std::uint64_t number = 0;
  std::uint64_t file_size = 0;
  std::string smallest;  // internal keys
  std::string largest;
};

// A change to the set of files, and to the counters that must survive a
// restart. Serialised as one MANIFEST record (docs/format/manifest.md).
class VersionEdit {
 public:
  void SetComparatorName(Slice name) { comparator_ = std::string(name); }
  void SetLogNumber(std::uint64_t number) { log_number_ = number; }
  void SetNextFile(std::uint64_t number) { next_file_number_ = number; }
  void SetLastSequence(SequenceNumber sequence) { last_sequence_ = sequence; }

  void AddFile(int level, FileMetaData file) { new_files_.emplace_back(level, std::move(file)); }
  void RemoveFile(int level, std::uint64_t number) { deleted_files_.emplace(level, number); }

  [[nodiscard]] const std::optional<std::string>& comparator() const { return comparator_; }
  [[nodiscard]] std::optional<std::uint64_t> log_number() const { return log_number_; }
  [[nodiscard]] std::optional<std::uint64_t> next_file_number() const { return next_file_number_; }
  [[nodiscard]] std::optional<SequenceNumber> last_sequence() const { return last_sequence_; }
  [[nodiscard]] const std::vector<std::pair<int, FileMetaData>>& new_files() const {
    return new_files_;
  }
  [[nodiscard]] const std::set<std::pair<int, std::uint64_t>>& deleted_files() const {
    return deleted_files_;
  }

  void EncodeTo(std::string* destination) const;
  // Replaces this edit with the one encoded in `input`. Corruption if malformed.
  Status DecodeFrom(Slice input);

 private:
  std::optional<std::string> comparator_;
  std::optional<std::uint64_t> log_number_;
  std::optional<std::uint64_t> next_file_number_;
  std::optional<SequenceNumber> last_sequence_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
  std::set<std::pair<int, std::uint64_t>> deleted_files_;
};

}  // namespace strata

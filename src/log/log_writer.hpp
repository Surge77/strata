#pragma once

#include "strata/env.hpp"
#include "strata/slice.hpp"
#include "strata/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "log/log_format.hpp"

namespace strata::log {

// Appends records to a log file in the format of docs/format/log.md.
//
// AddRecord flushes each record to the operating system, so a record survives
// a process crash as soon as AddRecord returns. Surviving power loss is the
// caller's decision: call Sync on the destination file (the WAL does so when a
// write asks for durability).
//
// After any failure the writer is poisoned and every later AddRecord returns
// the same error. A failed append may have written part of a fragment, so the
// writer's idea of the block offset no longer matches the file, and anything
// written after that would be misframed. The owner must start a new log.
class Writer {
 public:
  // `destination` must outlive the writer. `existing_length` is the file's
  // current size when appending to an existing log, so the writer resumes at
  // the right position within the current block.
  explicit Writer(WritableFile* destination, std::uint64_t existing_length = 0);

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer(Writer&&) = delete;
  Writer& operator=(Writer&&) = delete;
  ~Writer() = default;

  // Records may be any length, including empty.
  Status AddRecord(Slice record);

 private:
  Status EmitFragment(RecordType type, Slice payload);

  WritableFile* destination_;
  std::size_t block_offset_;
  Status error_;
  // CRC of each single type byte, precomputed: every fragment's checksum
  // starts from the checksum of its type byte and extends over the payload.
  std::array<std::uint32_t, kMaxRecordType + 1> type_crc_{};
};

}  // namespace strata::log

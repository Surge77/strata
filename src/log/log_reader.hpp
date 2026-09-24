#pragma once

#include "strata/env.hpp"
#include "strata/slice.hpp"
#include "strata/status.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "log/log_format.hpp"

namespace strata::log {

// Reads records back from a log written by log::Writer, following the reader
// contract in docs/format/log.md: a torn tail ends the log quietly, while real
// corruption is reported and skipped so one bad sector costs as little of the
// log as possible.
class Reader {
 public:
  // Told about every stretch of the log that had to be skipped. What to do
  // about it -- fail recovery or carry on -- is the caller's policy.
  class Reporter {
   public:
    Reporter() = default;
    virtual ~Reporter() = default;
    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;
    Reporter(Reporter&&) = delete;
    Reporter& operator=(Reporter&&) = delete;

    virtual void Corruption(std::size_t bytes_dropped, const Status& reason) = 0;
  };

  // `file` must outlive the reader; `reporter` may be null.
  Reader(SequentialFile* file, Reporter* reporter);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader(Reader&&) = delete;
  Reader& operator=(Reader&&) = delete;
  ~Reader() = default;

  // Reads the next complete record. On success sets `*record` -- which may
  // point into `*scratch` or into the reader's own buffer, and is valid until
  // the next call -- and returns true. Returns false at the end of the log.
  // An I/O error is reported to the Reporter and ends the log.
  bool ReadRecord(Slice* record, std::string* scratch);

  // File offset of the first header of the record most recently returned.
  [[nodiscard]] std::uint64_t LastRecordOffset() const noexcept { return last_record_offset_; }

 private:
  // Outcomes of ReadPhysicalRecord beyond the fragment types.
  enum Outcome : unsigned {
    kEof = kMaxRecordType + 1U,
    kBadRecord,  // skipped: already reported, or silent zero-fill
  };

  unsigned ReadPhysicalRecord(Slice* fragment);
  void ReportDrop(std::size_t bytes, Slice reason);

  SequentialFile* file_;
  Reporter* reporter_;
  std::unique_ptr<char[]> backing_store_;
  Slice buffer_;                            // unread bytes of the current block
  bool eof_ = false;                        // the last block read was short: no more data
  std::uint64_t end_of_buffer_offset_ = 0;  // file offset just past buffer_
  std::uint64_t last_record_offset_ = 0;
};

}  // namespace strata::log

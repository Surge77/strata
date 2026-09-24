#pragma once

#include "strata/env.hpp"
#include "strata/logger.hpp"
#include "strata/options.hpp"
#include "strata/result.hpp"
#include "strata/slice.hpp"
#include "strata/status.hpp"
#include "strata/write_batch.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "db/dbformat.hpp"
#include "db/memtable.hpp"
#include "log/log_writer.hpp"

namespace strata {

// The engine behind strata::DB.
//
// Phase 3 scope: everything lives in one memtable, and the write-ahead log is
// the only durable copy of the data. Open replays every log and then
// consolidates them into one fresh, synced log (see ConsolidateRecoveredLogs);
// phase 5 replaces that with flushing to SSTables. Concurrency is one mutex
// around every operation: correct, deliberately simple, and made fast in phase
// 9 (ADR-0004).
class DBImpl {
 public:
  static Result<std::unique_ptr<DBImpl>> Open(const Options& options);

  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;
  DBImpl(DBImpl&&) = delete;
  DBImpl& operator=(DBImpl&&) = delete;
  ~DBImpl();

  Status Write(const WriteOptions& options, const WriteBatch& batch);
  Result<std::string> Get(const ReadOptions& options, Slice key);

  // Closes the current log file and reports whether that succeeded. The
  // destructor releases the lock afterwards.
  Status Close();

 private:
  explicit DBImpl(const Options& options);

  Status OpenDatabase();
  // `newest` enables torn-tail handling for damage at the very end of the log.
  Status RecoverLogFile(std::uint64_t number, bool newest, SequenceNumber* max_sequence);
  // Rewrites everything recovered into the current (new) log, syncs it, then
  // deletes `old_logs`.
  Status ConsolidateRecoveredLogs(const std::vector<std::uint64_t>& old_logs);
  Status NewLogFile();

  const Options options_;
  Env* const env_;
  const std::string dbname_;

  // Declared first so it is released last, after the log file is closed.
  std::unique_ptr<FileLock> db_lock_;
  std::unique_ptr<Logger> owned_info_log_;
  Logger* info_log_ = nullptr;

  std::mutex mutex_;
  // Everything below is guarded by mutex_.
  std::unique_ptr<MemTable> mem_;
  std::unique_ptr<WritableFile> log_file_;  // outlives log_, which points into it
  std::unique_ptr<log::Writer> log_;
  std::uint64_t log_number_ = 0;
  std::uint64_t next_file_number_ = 1;
  SequenceNumber last_sequence_ = 0;
  // The first failed WAL write or sync. Once set, every later write fails
  // with it: the log may now hold a partial record, so appending more would
  // put acknowledged writes after a gap recovery would stop at.
  Status background_error_;
  // Set by Close. A thread that was already waiting for the mutex when another
  // thread closed the database gets InvalidArgument instead of a null log.
  bool closed_ = false;
};

}  // namespace strata

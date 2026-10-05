#pragma once

#include "strata/env.hpp"
#include "strata/logger.hpp"
#include "strata/options.hpp"
#include "strata/result.hpp"
#include "strata/slice.hpp"
#include "strata/status.hpp"
#include "strata/write_batch.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "db/dbformat.hpp"
#include "db/memtable.hpp"
#include "db/snapshot.hpp"
#include "db/table_cache.hpp"
#include "db/version_set.hpp"
#include "log/log_writer.hpp"

namespace strata {

// The engine behind strata::DB.
//
// Writes go to the WAL, then the active memtable. When the memtable fills it
// becomes the immutable memtable (with a fresh log for new writes) and a
// background thread flushes it to a level-0 SSTable, committing the new
// table through the MANIFEST. Reads check the memtable, the immutable one,
// then the current Version's tables. Concurrency is one mutex (ADR-0004); the
// flush does its I/O without holding it.
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

  const Snapshot* GetSnapshot();
  Status ReleaseSnapshot(const Snapshot* snapshot);

  // Stops the background thread, syncs and closes the log. The destructor
  // releases the lock afterwards.
  Status Close();

  // Test hook: blocks until no flush is pending or running.
  void WaitForFlush();

 private:
  explicit DBImpl(const Options& options);

  Status OpenDatabase();
  // `newest` enables torn-tail handling for damage at the very end of the log.
  Status RecoverLogFile(std::uint64_t number, bool newest, SequenceNumber* max_sequence);
  Status NewLogFile();
  // Flushes `mem` to a new level-0 table and records it in `edit`. Called with
  // mutex_ held; releases it around the I/O if `lock` is non-null.
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit, std::unique_lock<std::mutex>* lock);
  Status MakeRoomForWrite(std::unique_lock<std::mutex>* lock);
  void BackgroundLoop();
  void DeleteObsoleteFiles();

  const Options options_;
  Env* const env_;
  const std::string dbname_;
  const InternalKeyComparator internal_comparator_{BytewiseComparator()};

  // Declared first so it is released last.
  std::unique_ptr<FileLock> db_lock_;
  std::unique_ptr<Logger> owned_info_log_;
  Logger* info_log_ = nullptr;
  // Declared before table_cache_, whose tables point into it.
  std::unique_ptr<table::BlockCache> block_cache_;
  std::unique_ptr<TableCache> table_cache_;

  std::mutex mutex_;
  // Everything below is guarded by mutex_.
  std::unique_ptr<VersionSet> versions_;
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;  // being flushed, or waiting to be
  // Newest sequence number in imm_. What the flush commits as the MANIFEST's
  // last sequence: writes after it live only in the active log.
  SequenceNumber imm_last_sequence_ = 0;
  std::unique_ptr<WritableFile> log_file_;  // outlives log_, which points into it
  std::unique_ptr<log::Writer> log_;
  std::uint64_t log_number_ = 0;
  SequenceNumber last_sequence_ = 0;
  std::set<std::uint64_t> pending_outputs_;  // tables being built: never GC'd
  SnapshotList snapshots_;
  Status background_error_;
  bool closed_ = false;
  bool shutting_down_ = false;
  bool flush_running_ = false;
  std::condition_variable background_work_;
  std::condition_variable flush_done_;
  std::thread background_thread_;
};

}  // namespace strata

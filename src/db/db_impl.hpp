#pragma once

#include "strata/env.hpp"
#include "strata/logger.hpp"
#include "strata/options.hpp"
#include "strata/result.hpp"
#include "strata/slice.hpp"
#include "strata/status.hpp"
#include "strata/write_batch.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "db/compaction.hpp"
#include "db/dbformat.hpp"
#include "db/memtable.hpp"
#include "db/snapshot.hpp"
#include "db/table_cache.hpp"
#include "db/version_set.hpp"
#include "log/log_writer.hpp"

namespace strata {

// Counters for the work the engine has done, mainly to measure write
// amplification: bytes written to tables per byte of user data.
struct DBStats {
  std::uint64_t user_bytes_written = 0;   // encoded batches accepted by Write
  std::uint64_t flush_bytes_written = 0;  // level-0 tables written by flush
  std::uint64_t compactions = 0;
  std::uint64_t trivial_moves = 0;
  std::uint64_t compaction_bytes_read = 0;
  std::uint64_t compaction_bytes_written = 0;
  std::uint64_t write_slowdowns = 0;  // writes delayed because level 0 was long
  std::uint64_t write_stops = 0;      // waits because level 0 was full
  std::uint64_t write_groups = 0;     // log records written: one per commit group
  std::uint64_t log_syncs = 0;        // fsyncs of the log for synced groups
};

// The engine behind strata::DB.
//
// Writes go to the WAL, then the active memtable. When the memtable fills it
// becomes the immutable memtable (with a fresh log for new writes) and a
// background thread flushes it to a level-0 SSTable, committing the new
// table through the MANIFEST. The same thread compacts levels whenever one is
// over budget, flush first. Reads check the memtable, the immutable one, then
// the current Version's tables.
//
// Concurrency is one mutex (ADR-0004), held only to change shared state, never
// across I/O. Writers queue; the one at the front leads a commit group: it
// merges the batches queued behind it into one log record, appends and (if
// asked) fsyncs it, and inserts it into the memtable with the mutex released,
// then publishes the new sequence number. Only the leader touches the log and
// the memtable's write side, so those need no lock of their own.
class DBImpl {
 public:
  static Result<std::unique_ptr<DBImpl>> Open(const Options& options);
  // Test hook: opens with automatic compactions already paused, so none can
  // start between Open returning and PauseCompactionsForTest.
  static Result<std::unique_ptr<DBImpl>> OpenForTest(const Options& options,
                                                     bool compactions_paused);

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

  [[nodiscard]] DBStats GetStats();

  // Test hooks. ForTest methods may block and are not meant for production.
  std::size_t QueuedWritersForTest();         // writers waiting behind the leader
  void WaitForFlush();                        // until no flush is pending or running
  Status FlushMemTableForTest();              // flushes whatever the memtable holds
  Status CompactLevelForTest(int level);      // all of `level` into level + 1
  void WaitForCompactionsForTest();           // until no automatic compaction is due
  void PauseCompactionsForTest(bool paused);  // automatic ones only
  void PauseFlushesForTest(bool paused);      // the immutable memtable waits
  bool HasImmutableMemTableForTest();
  std::vector<FileMetaData> FilesAtLevelForTest(int level);
  std::uint64_t LevelBytesForTest(int level);
  void EvictTableForTest(std::uint64_t number);

 private:
  explicit DBImpl(const Options& options);

  Status OpenDatabase();
  // `newest` enables torn-tail handling for damage at the very end of the log.
  Status RecoverLogFile(std::uint64_t number, bool newest, SequenceNumber* max_sequence);
  Status NewLogFile();
  // Flushes `mem` to a new level-0 table and records it in `edit`. Called with
  // mutex_ held; releases it around the I/O if `lock` is non-null.
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit, std::unique_lock<std::mutex>* lock);
  // A Write waiting its turn, or leading a commit group.
  struct Writer {
    const WriteBatch* batch = nullptr;  // null: switch the memtable (flush hook)
    bool sync = false;
    bool done = false;
    Status status;
    std::condition_variable cv;
  };
  Status WriteImpl(const WriteOptions& options, const WriteBatch* batch);

  // The rest of these are called with mutex_ held.
  // `force` switches a non-empty memtable even if it has room.
  Status MakeRoomForWrite(std::unique_lock<std::mutex>* lock, bool force);
  // Merges the batches queued behind the leader into group_batch_ and points
  // *last_writer at the last writer included.
  void BuildBatchGroup(Writer** last_writer);
  // Makes the active memtable immutable, on a fresh log, for the flush.
  Status SwitchMemTable();
  void DeleteObsoleteFiles();

  // Background work, in db_impl_background.cpp.
  void BackgroundLoop();
  [[nodiscard]] bool HasBackgroundWork() const;
  [[nodiscard]] bool NeedsCompaction() const;
  void FlushImmutable(std::unique_lock<std::mutex>* lock);
  Status DoCompaction(const Compaction& compaction, std::unique_lock<std::mutex>* lock);
  // The oldest sequence number any reader may still ask for.
  [[nodiscard]] SequenceNumber SmallestSnapshot() const;

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
  std::deque<Writer*> writers_;           // front is the leader
  std::condition_variable writers_idle_;  // the queue just became empty
  WriteBatch group_batch_;                // the leader's merged batch; only the leader uses it
  DBStats stats_;
  // Per level, the user key after which the next automatic compaction starts.
  std::array<std::string, kNumLevels> compact_next_start_;
  std::optional<int> manual_compaction_level_;
  bool manual_compaction_done_ = false;
  Status manual_compaction_status_;
  bool compactions_paused_ = false;
  bool flushes_paused_ = false;
  Status background_error_;
  bool closed_ = false;
  bool shutting_down_ = false;
  bool background_running_ = false;
  std::condition_variable background_work_;
  std::condition_variable background_done_;  // a flush or compaction finished
  std::thread background_thread_;
};

}  // namespace strata

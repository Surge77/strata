#include "db/db_impl.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

#include "db/builder.hpp"
#include "db/db_iter.hpp"
#include "db/filename.hpp"
#include "db/write_batch_internal.hpp"
#include "log/log_reader.hpp"
#include "table/compression.hpp"
#include "table/merger.hpp"
#include "table/table_builder.hpp"

namespace strata {
namespace {

// Events kept for the stats API beyond those recorded during Open.
constexpr std::size_t kRecentEvents = 256;

template <typename Unit>
std::uint64_t Since(std::chrono::steady_clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<Unit>(std::chrono::steady_clock::now() - start).count());
}

std::uint64_t MicrosSince(std::chrono::steady_clock::time_point start) {
  return Since<std::chrono::microseconds>(start);
}

// Get and Write each time one call in this many per thread (see Get).
constexpr std::uint32_t kTimingInterval = 16;

// Where a new thread's count of calls starts, so threads begin their
// 1-in-kTimingInterval sampling at different offsets. Starting every thread
// at its first call would time each thread's cold first call, and with
// many short-lived threads those would crowd the sample. The stride is
// coprime with kTimingInterval, so consecutive threads spread out.
std::uint32_t FirstCallNumber() noexcept {
  constexpr std::uint32_t kStride = 5;
  static std::atomic<std::uint32_t> next{0};
  return next.fetch_add(kStride, std::memory_order_relaxed) % kTimingInterval;
}

// Latency histograms take nanoseconds (MetricsRegistry::AddLatency).
std::uint64_t NanosSince(std::chrono::steady_clock::time_point start) {
  return Since<std::chrono::nanoseconds>(start);
}

// Applies a batch's operations to the memtable with consecutive sequence
// numbers starting at the batch's own.
class MemTableInserter final : public WriteBatch::Handler {
 public:
  MemTableInserter(SequenceNumber first_sequence, MemTable* mem)
      : sequence_(first_sequence), mem_(mem) {}

  void Put(Slice key, Slice value) override {
    mem_->Add(sequence_++, ValueType::kValue, key, value);
  }
  void Delete(Slice key) override { mem_->Add(sequence_++, ValueType::kDeletion, key, Slice()); }

 private:
  SequenceNumber sequence_;
  MemTable* mem_;
};

// Discards the operations; used to validate a decoded batch completely before
// any of it touches the memtable, so a damaged batch is never half-applied.
class NullHandler final : public WriteBatch::Handler {
 public:
  void Put(Slice /*key*/, Slice /*value*/) override {}
  void Delete(Slice /*key*/) override {}
};

Status InsertInto(const WriteBatch& batch, MemTable* mem) {
  MemTableInserter inserter(WriteBatchInternal::Sequence(batch), mem);
  return batch.Iterate(&inserter);
}

// Prefixes a status message with the file it concerns, keeping the code.
Status InFile(const Status& status, const std::string& file) {
  const std::string message = file + ": " + std::string(status.message());
  return status.IsCorruption() ? Status::Corruption(message) : Status::IoError(message);
}

// Collects the log reader's reports while a WAL is replayed. Every report is
// logged; whether any is fatal is decided after the whole log has been read,
// because that depends on what follows the damage.
class RecoveryReporter final : public log::Reader::Reporter {
 public:
  RecoveryReporter(EventLog* events, std::string file) : events_(events), file_(std::move(file)) {}

  void Corruption(std::size_t bytes_dropped, const Status& reason) override {
    events_->Record(
        LogLevel::kWarn, "wal_corruption_skipped",
        {Field("file", file_), Field("bytes", bytes_dropped), Field("reason", reason.ToString())});
    if (reason.IsIoError() && io_error_.ok()) {
      io_error_ = InFile(reason, file_);
    }
    if (first_problem_.ok()) {
      first_problem_ = InFile(reason, file_);
    }
    ++problems_;
  }

  [[nodiscard]] const Status& first_problem() const noexcept { return first_problem_; }
  [[nodiscard]] const Status& io_error() const noexcept { return io_error_; }
  [[nodiscard]] std::uint64_t problems() const noexcept { return problems_; }

 private:
  EventLog* events_;
  std::string file_;
  Status first_problem_;
  Status io_error_;
  std::uint64_t problems_ = 0;
};

}  // namespace

DBImpl::DBImpl(const Options& options)
    : options_(options),
      env_(options.env != nullptr ? options.env : Env::Default()),
      dbname_(options.path),
      info_log_(options.info_log),
      events_(options.info_log, kRecentEvents) {}

DBImpl::~DBImpl() {
  static_cast<void>(Close());
}

Result<std::unique_ptr<DBImpl>> DBImpl::Open(const Options& options) {
  return OpenForTest(options, /*compactions_paused=*/false);
}

Result<std::unique_ptr<DBImpl>> DBImpl::OpenForTest(const Options& options,
                                                    bool compactions_paused) {
  if (options.path.empty()) {
    return Status::InvalidArgument("Options::path is empty");
  }
  const auto compression = static_cast<table::CompressionType>(options.compression);
  if (compression != table::CompressionType::kNone &&
      table::CompressorFor(compression) == nullptr) {
    return Status::NotSupported(std::string("this build leaves out ") +
                                table::CompressionName(compression) + " compression");
  }
  // The constructor is private, so make_unique cannot reach it.
  std::unique_ptr<DBImpl> impl(new DBImpl(options));
  impl->compactions_paused_ = compactions_paused;  // before the thread starts
  STRATA_RETURN_IF_ERROR(impl->OpenDatabase());
  return impl;
}

Status DBImpl::OpenDatabase() {
  if (!env_->FileExists(dbname_)) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument(dbname_ + " does not exist (create_if_missing is false)");
    }
    STRATA_RETURN_IF_ERROR(env_->CreateDir(dbname_));
  }

  // The lock comes first: nothing else in the directory may be touched while
  // another process has the database open.
  STRATA_ASSIGN_OR_RETURN(db_lock_, env_->LockFile(LockFileName(dbname_)));

  table_cache_ =
      std::make_shared<TableCache>(dbname_, env_, &internal_comparator_, options_.max_open_tables,
                                   std::make_shared<table::BlockCache>(options_.block_cache_size));
  versions_ = std::make_unique<VersionSet>(dbname_, env_, &internal_comparator_);
  bool existing = false;
  STRATA_RETURN_IF_ERROR(versions_->Recover(&existing));

  STRATA_ASSIGN_OR_RETURN(const std::vector<std::string> children, env_->GetChildren(dbname_));
  std::vector<std::uint64_t> logs;
  for (const std::string& child : children) {
    std::uint64_t number = 0;
    FileType type = FileType::kLockFile;
    if (!ParseFileName(child, &number, &type)) {
      continue;
    }
    if (number == UINT64_MAX) {
      // The next file would need number UINT64_MAX + 1, which wraps to 0.
      return Status::Corruption(dbname_ + "/" + child + ": file number space exhausted");
    }
    versions_->MarkFileNumberUsed(number);
    // Logs below the MANIFEST's log number were flushed to tables already.
    if (type == FileType::kLogFile && number >= versions_->log_number()) {
      logs.push_back(number);
    }
    existing = existing || type == FileType::kLogFile || type == FileType::kTableFile;
  }
  // Checked before anything is written, so rejecting an existing database
  // leaves it exactly as it was.
  if (options_.error_if_exists && existing) {
    return Status::AlreadyExists(dbname_ + " already contains a database (error_if_exists)");
  }

  if (info_log_ == nullptr) {
    // Appended to, never rotated, so a crashed session's log survives
    // repeated failed attempts to reopen.
    STRATA_ASSIGN_OR_RETURN(owned_info_log_, NewFileLogger(env_, InfoLogFileName(dbname_)));
    info_log_ = owned_info_log_.get();
    events_.SetLogger(info_log_);
  }
  events_.Record(LogLevel::kInfo, "db_opening", {Field("path", dbname_)});

  // Replay, in creation order, every log not yet flushed to a table.
  std::sort(logs.begin(), logs.end());
  mem_ = std::make_shared<MemTable>(&internal_comparator_);
  SequenceNumber max_sequence = versions_->last_sequence();
  for (std::size_t index = 0; index < logs.size(); ++index) {
    const bool newest = index + 1 == logs.size();
    STRATA_RETURN_IF_ERROR(RecoverLogFile(logs[index], newest, &max_sequence));
  }
  last_sequence_ = max_sequence;
  versions_->SetLastSequence(last_sequence_);

  // Whatever was replayed becomes a level-0 table, committed together with
  // the new log number in one MANIFEST edit. Until that commit the old logs
  // stay authoritative; after it they are obsolete. This also starts the
  // session's new MANIFEST and publishes it through CURRENT.
  STRATA_RETURN_IF_ERROR(NewLogFile(/*lock=*/nullptr));
  VersionEdit edit;
  if (!mem_->empty()) {
    STRATA_RETURN_IF_ERROR(WriteLevel0Table(mem_.get(), &edit, nullptr));
    mem_ = std::make_shared<MemTable>(&internal_comparator_);
  }
  edit.SetLogNumber(log_number_);
  STRATA_RETURN_IF_ERROR(versions_->LogAndApply(&edit));
  DeleteObsoleteFiles();

  background_thread_ = std::thread([this] { BackgroundLoop(); });
  events_.Record(LogLevel::kInfo, "db_opened",
                 {Field("logs_replayed", logs.size()), Field("last_sequence", last_sequence_)});
  events_.MarkOpened();
  return Status::Ok();
}

Status DBImpl::RecoverLogFile(std::uint64_t number, bool newest, SequenceNumber* max_sequence) {
  const std::string file_name = LogFileName(dbname_, number);
  STRATA_ASSIGN_OR_RETURN(auto file, env_->NewSequentialFile(file_name));
  RecoveryReporter reporter(&events_, file_name);
  log::Reader reader(file.get(), &reporter);

  Slice record;
  std::string scratch;
  WriteBatch batch;
  std::uint64_t applied = 0;
  std::uint64_t applied_after_problem = 0;
  while (reader.ReadRecord(&record, &scratch)) {
    if (!WriteBatchInternal::SetContents(&batch, record).ok()) {
      reporter.Corruption(record.size(), Status::Corruption("log record shorter than a batch"));
      continue;
    }
    // Validate the whole batch before applying any of it: a batch is atomic,
    // so a damaged one is dropped entirely rather than half-applied.
    NullHandler validator;
    if (const Status valid = batch.Iterate(&validator); !valid.ok()) {
      reporter.Corruption(record.size(), valid);
      continue;
    }
    if (batch.Count() == 0) {
      continue;  // never written by Write; carries nothing to recover
    }
    const SequenceNumber first = WriteBatchInternal::Sequence(batch);
    const SequenceNumber last = first + batch.Count() - 1;
    if (last <= *max_sequence) {
      continue;  // already durable in a table: this log outlived its flush
    }
    if (first <= *max_sequence) {
      reporter.Corruption(record.size(),
                          Status::Corruption("batch overlaps already-recovered sequence numbers"));
      continue;
    }
    STRATA_RETURN_IF_ERROR(InsertInto(batch, mem_.get()));
    *max_sequence = last;
    ++applied;
    if (reporter.problems() > 0) {
      ++applied_after_problem;
    }
  }

  STRATA_RETURN_IF_ERROR(reporter.io_error());
  if (reporter.problems() > 0) {
    if (newest && applied_after_problem == 0 && !reader.SkippedValidRecord()) {
      // Damage with nothing valid after it, at the end of the newest log, is
      // what an interrupted append looks like -- including after power loss,
      // when a record's first block can survive while the next reads back as
      // zeros. Nothing in it was acknowledged as durable. "Nothing valid"
      // includes the bytes the reader skipped: in a log of one partial block,
      // records after the damage are never applied, only skipped.
      events_.Record(LogLevel::kWarn, "wal_torn_tail", {Field("file", file_name)});
    } else if (!options_.ignore_wal_corruption) {
      return reporter.first_problem();
    }
  }
  events_.Record(LogLevel::kInfo, "wal_replayed",
                 {Field("log", number), Field("batches", applied)});
  return Status::Ok();
}

Status DBImpl::NewLogFile(std::unique_lock<std::mutex>* lock) {
  const std::uint64_t number = versions_->NewFileNumber();
  const std::string file_name = LogFileName(dbname_, number);
  WritableFile* const retired = log_file_.get();
  // The I/O below can take tens of milliseconds (the retired log may hold
  // write_buffer_size unsynced bytes), so it runs without the mutex. That is
  // safe because only the leading writer rotates and only it touches the log:
  // queued writers wait their turn, readers keep using the current memtable,
  // which takes no writes meanwhile, Close waits for the writer queue to
  // drain, and obsolete-file cleanup keeps every log at or above the
  // committed log number, which this one is.
  if (lock != nullptr) {
    lock->unlock();
  }
  auto created = [&]() -> Result<std::unique_ptr<WritableFile>> {
    STRATA_ASSIGN_OR_RETURN(auto file, env_->NewWritableFile(file_name));
    // Make the new log's *name* durable before any write relies on it.
    STRATA_RETURN_IF_ERROR(env_->SyncDir(dbname_));
    if (retired != nullptr) {
      // Sync the log being retired before a newer one can take synced
      // writes: otherwise power loss could keep a later synced write while
      // losing earlier unsynced ones, leaving a state that is not a prefix of
      // what was acknowledged.
      STRATA_RETURN_IF_ERROR(retired->Sync());
    }
    return file;
  }();
  if (lock != nullptr) {
    lock->lock();
  }
  if (!created.ok()) {
    return created.status();
  }
  log_ = nullptr;
  log_file_ = std::move(*created);
  log_ = std::make_unique<log::Writer>(log_file_.get());
  log_number_ = number;
  if (retired != nullptr) {
    ++stats_.log_rotations;
    events_.Record(LogLevel::kInfo, "log_rotated", {Field("log", number)});
  }
  return Status::Ok();
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                                std::unique_lock<std::mutex>* lock) {
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  auto it = mem->NewIterator();
  const auto start = std::chrono::steady_clock::now();
  if (lock != nullptr) {
    lock->unlock();
  }
  const table::TableOptions table_options{
      .comparator = &internal_comparator_,
      .use_filter = options_.bloom_filter,
      .compression = static_cast<table::CompressionType>(options_.compression),
      .compression_level = options_.compression_level,
  };
  Status status = BuildTable(dbname_, env_, table_options, table_cache_.get(), it.get(), &meta);
  if (lock != nullptr) {
    lock->lock();
  }
  pending_outputs_.erase(meta.number);
  STRATA_RETURN_IF_ERROR(status);
  if (meta.file_size > 0) {
    events_.Record(LogLevel::kInfo, "flush_finished",
                   {Field("table", meta.number), Field("bytes", meta.file_size),
                    Field("micros", MicrosSince(start))});
    ++stats_.flushes;
    stats_.flush_bytes_written += meta.file_size;
    edit->AddFile(0, std::move(meta));
  }
  return Status::Ok();
}

void DBImpl::DeleteObsoleteFiles() {
  std::set<std::uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);
  auto children = env_->GetChildren(dbname_);
  if (!children.ok()) {
    return;  // retried after the next flush
  }
  for (const std::string& child : *children) {
    std::uint64_t number = 0;
    FileType type = FileType::kLockFile;
    if (!ParseFileName(child, &number, &type)) {
      continue;
    }
    bool keep = true;
    switch (type) {
      case FileType::kLogFile:
        keep = number >= versions_->log_number();
        break;
      case FileType::kDescriptorFile:
        keep = number >= versions_->manifest_file_number();
        break;
      case FileType::kTableFile:
        keep = live.contains(number);
        break;
      case FileType::kTempFile:
        keep = false;  // a CURRENT that was never renamed into place
        break;
      case FileType::kCurrentFile:
      case FileType::kLockFile:
      case FileType::kInfoLogFile:
        break;
    }
    if (!keep) {
      if (type == FileType::kTableFile) {
        table_cache_->Evict(number);
      }
      Log(info_log_, LogLevel::kInfo, "deleting obsolete file {}", child);
      static_cast<void>(env_->RemoveFile(dbname_ + "/" + child));
    }
  }
}

Status DBImpl::Write(const WriteOptions& options, const WriteBatch& batch) {
  if (batch.oversized()) {
    return Status::InvalidArgument("key or value exceeds the size limit (see WriteBatch)");
  }
  if (batch.Count() == 0) {
    return Status::Ok();
  }
  // Sampled like Get: two clock reads cost about 4% of an unsynced write
  // (phase 12 A/B).
  thread_local std::uint32_t writes_on_this_thread = FirstCallNumber();
  const bool timed = writes_on_this_thread++ % kTimingInterval == 0;
  const auto start =
      timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  Status status = WriteImpl(options, &batch);
  // Only accepted writes: a refused one is not part of the group-commit
  // factor write.calls / write.groups.
  if (status.ok()) {
    hot_.write_calls.Add();
    if (timed) {
      hot_.write_nanos.Add(NanosSince(start));
    }
  }
  return status;
}

Status DBImpl::WriteImpl(const WriteOptions& options, const WriteBatch* batch) {
  Writer self;
  self.batch = batch;
  self.sync = options.sync;
  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_) {
    return Status::InvalidArgument("database is closed");
  }
  writers_.push_back(&self);
  self.cv.wait(lock, [&] { return self.done || writers_.front() == &self; });
  if (self.done) {
    return self.status;  // a leader committed this write in its group
  }

  // This writer leads: it commits itself and the writers queued behind it.
  Status status = MakeRoomForWrite(&lock, /*force=*/batch == nullptr);
  Writer* last_writer = &self;
  if (status.ok() && batch != nullptr) {
    BuildBatchGroup(&last_writer);
    const SequenceNumber first = last_sequence_ + 1;
    WriteBatchInternal::SetSequence(&group_batch_, first);
    log::Writer* const log = log_.get();
    WritableFile* const log_file = log_file_.get();
    MemTable* const mem = mem_.get();
    // Only an fsync is slow enough to be worth releasing the mutex for. An
    // unsynced group's I/O is a copy into the page cache; dropping and
    // retaking the mutex, and waking each follower, cost more than that copy
    // (measured: 8 unsynced writers fell from 255k to 57k writes/s).
    const bool release_mutex = self.sync;
    if (release_mutex) {
      lock.unlock();
    }
    // The durability boundary: once the record is in the log (and synced, if
    // the group asked), its writes are acknowledged. They become visible only
    // when last_sequence_ moves past them, below.
    status = log->AddRecord(WriteBatchInternal::Contents(group_batch_));
    if (status.ok() && self.sync) {
      status = log_file->Sync();
    }
    if (status.ok()) {
      status = InsertInto(group_batch_, mem);
    }
    if (release_mutex && after_memtable_insert_hook_) {
      after_memtable_insert_hook_();
    }
    if (release_mutex) {
      lock.lock();
    }
    if (status.ok()) {
      last_sequence_ = first + WriteBatchInternal::Count(group_batch_) - 1;
      ++stats_.write_groups;
      stats_.log_syncs += self.sync ? 1U : 0U;
      stats_.user_bytes_written += WriteBatchInternal::Contents(group_batch_).size();
    } else {
      background_error_ = status;
      background_done_.notify_all();  // waiters re-check background_error_
      events_.Record(LogLevel::kError, "write_failed",
                     {Field("log", log_number_), Field("error", status.ToString())});
    }
    group_batch_.Clear();
  }

  // Hand the outcome to every writer in the group, then wake the next leader.
  while (true) {
    Writer* const ready = writers_.front();
    writers_.pop_front();
    if (ready != &self) {
      ready->status = status;
      ready->done = true;
      ready->cv.notify_one();
    }
    if (ready == last_writer) {
      break;
    }
  }
  if (!writers_.empty()) {
    writers_.front()->cv.notify_one();
  } else {
    writers_idle_.notify_all();
  }
  return status;
}

void DBImpl::BuildBatchGroup(Writer** last_writer) {
  // A group of small writes may grow by this much; a large write is not held
  // back by more than this. The cap also keeps the merged count far below
  // the batch format's 2^32 limit.
  constexpr std::size_t kSmallWriteBytes = 128 * 1024;
  constexpr std::size_t kMaxGroupBytes = 1024 * 1024;

  const Writer* const first = writers_.front();
  group_batch_ = *first->batch;  // a copy: the caller's batch stays untouched
  std::size_t size = group_batch_.ApproximateSize();
  const std::size_t max_size = size <= kSmallWriteBytes ? size + kSmallWriteBytes : kMaxGroupBytes;
  for (auto it = writers_.begin() + 1; it != writers_.end(); ++it) {
    Writer* const writer = *it;
    if (writer->sync && !first->sync) {
      break;  // an unsynced group must not acknowledge a synced write
    }
    if (writer->batch == nullptr) {
      break;  // a forced memtable switch is a group of its own
    }
    size += writer->batch->ApproximateSize();
    if (size > max_size) {
      break;
    }
    // Append refuses a count overflow without applying the batch; this
    // writer would then be acknowledged unwritten. The size cap makes that
    // unreachable, but the check keeps it impossible rather than unlikely.
    if (WriteBatchInternal::Count(group_batch_) >
        std::numeric_limits<std::uint32_t>::max() - writer->batch->Count()) {
      break;
    }
    group_batch_.Append(*writer->batch);
    *last_writer = writer;
  }
}

std::size_t DBImpl::QueuedWritersForTest() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return writers_.empty() ? 0 : writers_.size() - 1;
}

Result<std::string> DBImpl::Get(const ReadOptions& options, Slice key) {
  // Two clock reads cost about 50 ns, over 10% of a Get that misses every
  // table through its Bloom filter (phase 12 A/B). Timing one call in
  // kTimingInterval per thread keeps a representative sample at a
  // sixteenth of that cost. Each thread starts at its own offset
  // (FirstCallNumber).
  thread_local std::uint32_t calls_on_this_thread = FirstCallNumber();
  const bool timed = calls_on_this_thread++ % kTimingInterval == 0;
  const auto start =
      timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  Result<std::string> result = GetImpl(options, key);
  // A call refused before any lookup (oversized key, closed database,
  // foreign snapshot) is not a read.
  if (result.ok() || !result.status().IsInvalidArgument()) {
    hot_.get_calls.Add();
    if (result.ok()) {
      hot_.get_found.Add();
    }
    if (timed) {
      hot_.get_nanos.Add(NanosSince(start));
    }
  }
  return result;
}

Result<std::string> DBImpl::GetImpl(const ReadOptions& options, Slice key) {
  if (key.size() > WriteBatch::kMaxKeySize) {
    return Status::InvalidArgument("key exceeds the size limit");
  }
  std::shared_ptr<MemTable> mem;
  std::shared_ptr<MemTable> imm;
  std::shared_ptr<const Version> version;
  SequenceNumber sequence = 0;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (closed_) {
      return Status::InvalidArgument("database is closed");
    }
    mem = mem_;
    imm = imm_;
    version = versions_->current();
    sequence = last_sequence_;
    if (options.snapshot != nullptr) {
      const SnapshotImpl* snapshot = snapshots_.Find(options.snapshot);
      if (snapshot == nullptr) {
        return Status::InvalidArgument("snapshot was released or belongs to another database");
      }
      sequence = snapshot->sequence();
    }
  }
  // Searched without the mutex: the memtables allow concurrent readers, and
  // the Version (and so its files) stays alive while `version` is held.
  const LookupKey lookup(key, sequence);
  std::string value;
  for (const MemTable* table : {mem.get(), imm.get()}) {
    if (table == nullptr) {
      continue;
    }
    switch (table->Get(lookup, &value)) {
      case MemTable::LookupResult::kFound:
        hot_.get_memtable_answers.Add();
        return value;
      case MemTable::LookupResult::kDeleted:
        hot_.get_memtable_answers.Add();
        return Status::NotFound();
      case MemTable::LookupResult::kNotPresent:
        break;
    }
  }
  ReadCounters counters;
  const auto found = version->Get(lookup, &value, table_cache_.get(), &counters);
  // Skipping zero adds saves a thread-local read and a store per lookup.
  if (counters.table_reads != 0) {
    hot_.get_table_reads.Add(counters.table_reads);
  }
  if (counters.filter_skips != 0) {
    hot_.get_filter_skips.Add(counters.filter_skips);
  }
  STRATA_RETURN_IF_ERROR(found.status());
  if (*found == MemTable::LookupResult::kFound) {
    return value;
  }
  return Status::NotFound();
}

const Snapshot* DBImpl::GetSnapshot() {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (closed_) {
    return nullptr;
  }
  return snapshots_.New(last_sequence_);
}

Status DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (!snapshots_.Release(snapshot)) {
    return Status::InvalidArgument("snapshot was already released or belongs to another database");
  }
  return Status::Ok();
}

Status DBImpl::FlushMemTableForTest() {
  // Through the writer queue, so the switch never races a leader's log write.
  STRATA_RETURN_IF_ERROR(WriteImpl(WriteOptions{}, nullptr));
  std::unique_lock<std::mutex> lock(mutex_);
  background_done_.wait(lock,
                        [this] { return imm_ == nullptr || !background_error_.ok() || closed_; });
  if (closed_ && imm_ != nullptr) {
    return Status::InvalidArgument("database closed before the flush finished");
  }
  return background_error_;
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& options) {
  hot_.iterators.Add();
  std::shared_ptr<MemTable> mem;
  std::shared_ptr<MemTable> imm;
  std::shared_ptr<const Version> version;
  SequenceNumber sequence = 0;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (closed_) {
      return NewErrorIterator(Status::InvalidArgument("database is closed"));
    }
    mem = mem_;
    imm = imm_;
    version = versions_->current();
    sequence = last_sequence_;
    if (options.snapshot != nullptr) {
      const SnapshotImpl* snapshot = snapshots_.Find(options.snapshot);
      if (snapshot == nullptr) {
        return NewErrorIterator(
            Status::InvalidArgument("snapshot was released or belongs to another database"));
      }
      sequence = snapshot->sequence();
    }
  }
  // The iterator holds the memtables, the Version (and so its files), and the
  // table cache: it is a snapshot in its own right, and stays usable after
  // the snapshot is released or even after the database is closed.
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(mem->NewIterator());
  if (imm != nullptr) {
    children.push_back(imm->NewIterator());
  }
  if (Status status = version->AddIterators(table_cache_.get(), &children); !status.ok()) {
    return NewErrorIterator(status);
  }
  std::vector<std::shared_ptr<const void>> keep_alive{mem, version, table_cache_};
  if (imm != nullptr) {
    keep_alive.push_back(imm);
  }
  return NewDBIterator(internal_comparator_.user_comparator(),
                       table::NewMergingIterator(&internal_comparator_, std::move(children)),
                       sequence,
                       DBIterBounds{.lower = options.lower_bound, .upper = options.upper_bound},
                       std::move(keep_alive));
}

void DBImpl::WaitForFlush() {
  std::unique_lock<std::mutex> lock(mutex_);
  background_done_.wait(lock, [this] {
    return (imm_ == nullptr && !background_running_) || !background_error_.ok() || shutting_down_;
  });
}

Status DBImpl::Close() {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (closed_) {
      return Status::Ok();
    }
    closed_ = true;
    shutting_down_ = true;
  }
  background_work_.notify_all();
  // Writers stalled in MakeRoomForWrite and test hooks wait for background
  // work that will now never finish; wake them so they see the close.
  background_done_.notify_all();
  {
    // A leader may be writing the log with the mutex released. Each queued
    // writer becomes leader in turn, sees closed_, and leaves; the log is
    // closed only once the queue is empty.
    std::unique_lock<std::mutex> lock(mutex_);
    writers_idle_.wait(lock, [this] { return writers_.empty(); });
  }
  if (background_thread_.joinable()) {
    background_thread_.join();
  }

  const std::lock_guard<std::mutex> guard(mutex_);
  Status status;
  if (log_file_ != nullptr) {
    // A clean close leaves every acknowledged write durable, whatever its
    // WriteOptions: one fdatasync, paid once per session. An unflushed
    // memtable is simply replayed from the log next time.
    status = log_file_->Sync();
    if (status.ok() && !background_error_.ok()) {
      status = background_error_;
    }
    log_ = nullptr;
    const Status closed = log_file_->Close();
    if (status.ok()) {
      status = closed;
    }
    log_file_ = nullptr;
    events_.Record(LogLevel::kInfo, "db_closed");
  }
  return status;
}

}  // namespace strata

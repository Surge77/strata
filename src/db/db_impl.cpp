#include "db/db_impl.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "db/builder.hpp"
#include "db/filename.hpp"
#include "db/write_batch_internal.hpp"
#include "log/log_reader.hpp"

namespace strata {
namespace {

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
  RecoveryReporter(Logger* info_log, std::string file)
      : info_log_(info_log), file_(std::move(file)) {}

  void Corruption(std::size_t bytes_dropped, const Status& reason) override {
    Log(info_log_, LogLevel::kWarn, "{}: dropping {} bytes: {}", file_, bytes_dropped,
        reason.ToString());
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
  Logger* info_log_;
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
      info_log_(options.info_log) {}

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

  block_cache_ = std::make_unique<table::BlockCache>(options_.block_cache_size);
  table_cache_ = std::make_unique<TableCache>(dbname_, env_, &internal_comparator_,
                                              options_.max_open_tables, block_cache_.get());
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
  }
  Log(info_log_, LogLevel::kInfo, "=== opening {} ===", dbname_);

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
  STRATA_RETURN_IF_ERROR(NewLogFile());
  VersionEdit edit;
  if (!mem_->empty()) {
    STRATA_RETURN_IF_ERROR(WriteLevel0Table(mem_.get(), &edit, nullptr));
    mem_ = std::make_shared<MemTable>(&internal_comparator_);
  }
  edit.SetLogNumber(log_number_);
  STRATA_RETURN_IF_ERROR(versions_->LogAndApply(&edit));
  DeleteObsoleteFiles();

  background_thread_ = std::thread([this] { BackgroundLoop(); });
  Log(info_log_, LogLevel::kInfo, "opened {}: replayed {} log(s), last sequence {}", dbname_,
      logs.size(), last_sequence_);
  return Status::Ok();
}

Status DBImpl::RecoverLogFile(std::uint64_t number, bool newest, SequenceNumber* max_sequence) {
  const std::string file_name = LogFileName(dbname_, number);
  STRATA_ASSIGN_OR_RETURN(auto file, env_->NewSequentialFile(file_name));
  RecoveryReporter reporter(info_log_, file_name);
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
    if (newest && applied_after_problem == 0) {
      // Damage with nothing valid after it, at the end of the newest log, is
      // what an interrupted append looks like -- including after power loss,
      // when a record's first block can survive while the next reads back as
      // zeros. Nothing in it was acknowledged as durable.
      Log(info_log_, LogLevel::kWarn,
          "{}: treating damage at the end of the newest log as a torn write", file_name);
    } else if (!options_.ignore_wal_corruption) {
      return reporter.first_problem();
    }
  }
  Log(info_log_, LogLevel::kInfo, "{}: {} batch(es) applied", file_name, applied);
  return Status::Ok();
}

Status DBImpl::NewLogFile() {
  const std::uint64_t number = versions_->NewFileNumber();
  const std::string file_name = LogFileName(dbname_, number);
  STRATA_ASSIGN_OR_RETURN(auto file, env_->NewWritableFile(file_name));
  // Make the new log's *name* durable before any write relies on it.
  STRATA_RETURN_IF_ERROR(env_->SyncDir(dbname_));
  if (log_file_ != nullptr) {
    // Sync the log being retired before a newer one can take synced writes:
    // otherwise power loss could keep a later synced write while losing
    // earlier unsynced ones, leaving a state that is not a prefix of what was
    // acknowledged.
    STRATA_RETURN_IF_ERROR(log_file_->Sync());
  }
  log_ = nullptr;
  log_file_ = std::move(file);
  log_ = std::make_unique<log::Writer>(log_file_.get());
  log_number_ = number;
  return Status::Ok();
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                                std::unique_lock<std::mutex>* lock) {
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  auto it = mem->NewIterator();
  if (lock != nullptr) {
    lock->unlock();
  }
  Status status = BuildTable(dbname_, env_, &internal_comparator_, options_.bloom_filter,
                             table_cache_.get(), it.get(), &meta);
  if (lock != nullptr) {
    lock->lock();
  }
  pending_outputs_.erase(meta.number);
  STRATA_RETURN_IF_ERROR(status);
  if (meta.file_size > 0) {
    Log(info_log_, LogLevel::kInfo, "flushed table {} ({} bytes)", meta.number, meta.file_size);
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
  return WriteImpl(options, &batch);
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
      Log(info_log_, LogLevel::kError, "write to log {} failed; refusing further writes: {}",
          log_number_, status.ToString());
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
    group_batch_.Append(*writer->batch);
    *last_writer = writer;
  }
}

std::size_t DBImpl::QueuedWritersForTest() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return writers_.empty() ? 0 : writers_.size() - 1;
}

Result<std::string> DBImpl::Get(const ReadOptions& options, Slice key) {
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
        return value;
      case MemTable::LookupResult::kDeleted:
        return Status::NotFound();
      case MemTable::LookupResult::kNotPresent:
        break;
    }
  }
  STRATA_ASSIGN_OR_RETURN(const auto found, version->Get(lookup, &value, table_cache_.get()));
  if (found == MemTable::LookupResult::kFound) {
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
  background_done_.wait(lock, [this] { return imm_ == nullptr || !background_error_.ok(); });
  return background_error_;
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
    Log(info_log_, LogLevel::kInfo, "closed {}", dbname_);
  }
  return status;
}

}  // namespace strata

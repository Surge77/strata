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

bool IsEmpty(const MemTable& mem) {
  auto it = mem.NewIterator();
  it->SeekToFirst();
  return !it->Valid();
}

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
  if (options.path.empty()) {
    return Status::InvalidArgument("Options::path is empty");
  }
  // The constructor is private, so make_unique cannot reach it.
  std::unique_ptr<DBImpl> impl(new DBImpl(options));
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

  table_cache_ = std::make_unique<TableCache>(dbname_, env_, &internal_comparator_);
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
  if (!IsEmpty(*mem_)) {
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
  Status status =
      BuildTable(dbname_, env_, &internal_comparator_, table_cache_.get(), it.get(), &meta);
  if (lock != nullptr) {
    lock->lock();
  }
  pending_outputs_.erase(meta.number);
  STRATA_RETURN_IF_ERROR(status);
  if (meta.file_size > 0) {
    Log(info_log_, LogLevel::kInfo, "flushed table {} ({} bytes)", meta.number, meta.file_size);
    edit->AddFile(0, std::move(meta));
  }
  return Status::Ok();
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>* lock) {
  while (true) {
    if (!background_error_.ok()) {
      return background_error_;
    }
    if (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size) {
      return Status::Ok();
    }
    if (imm_ != nullptr) {
      // The previous memtable is still being flushed: wait for it. This is
      // the (simple) write stall; phase 7 adds level-0 slowdowns.
      flush_done_.wait(*lock);
      continue;
    }
    if (Status status = NewLogFile(); !status.ok()) {
      background_error_ = status;
      return status;
    }
    imm_ = std::move(mem_);
    mem_ = std::make_shared<MemTable>(&internal_comparator_);
    background_work_.notify_one();
  }
}

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    background_work_.wait(
        lock, [this] { return shutting_down_ || (imm_ != nullptr && background_error_.ok()); });
    if (shutting_down_) {
      return;
    }
    flush_running_ = true;
    const std::shared_ptr<MemTable> imm = imm_;
    VersionEdit edit;
    Status status = WriteLevel0Table(imm.get(), &edit, &lock);
    if (status.ok()) {
      // Logs before the active one hold only what `imm` held, which is now in
      // a table: committing the active log number retires them.
      edit.SetLogNumber(log_number_);
      versions_->SetLastSequence(last_sequence_);
      status = versions_->LogAndApply(&edit);
    }
    if (status.ok()) {
      imm_ = nullptr;
      DeleteObsoleteFiles();
    } else {
      background_error_ = status;
      Log(info_log_, LogLevel::kError, "flush failed; refusing further writes: {}",
          status.ToString());
    }
    flush_running_ = false;
    flush_done_.notify_all();
  }
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

  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_) {
    return Status::InvalidArgument("database is closed");
  }
  STRATA_RETURN_IF_ERROR(MakeRoomForWrite(&lock));

  // Stamp a copy with its sequence number; the caller's batch stays untouched.
  WriteBatch sequenced = batch;
  WriteBatchInternal::SetSequence(&sequenced, last_sequence_ + 1);

  // The durability boundary: once the record is in the log (and synced, if
  // asked), the write is acknowledged. Only then is it made visible.
  Status status = log_->AddRecord(WriteBatchInternal::Contents(sequenced));
  if (status.ok() && options.sync) {
    status = log_file_->Sync();
  }
  if (status.ok()) {
    status = InsertInto(sequenced, mem_.get());
  }
  if (!status.ok()) {
    background_error_ = status;
    Log(info_log_, LogLevel::kError, "write to log {} failed; refusing further writes: {}",
        log_number_, status.ToString());
    return status;
  }
  last_sequence_ += sequenced.Count();
  return Status::Ok();
}

Result<std::string> DBImpl::Get(const ReadOptions& /*options*/, Slice key) {
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

void DBImpl::WaitForFlush() {
  std::unique_lock<std::mutex> lock(mutex_);
  flush_done_.wait(lock, [this] {
    return (imm_ == nullptr && !flush_running_) || !background_error_.ok() || shutting_down_;
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
  if (background_thread_.joinable()) {
    background_thread_.join();
  }

  const std::lock_guard<std::mutex> guard(mutex_);
  Status status;
  if (log_file_ != nullptr) {
    // A clean close leaves every acknowledged write durable, whatever its
    // WriteOptions: one fdatasync, paid once per session. An unflushed
    // memtable is simply replayed from the log next time.
    if (background_error_.ok()) {
      status = log_file_->Sync();
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

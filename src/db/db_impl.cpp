#include "db/db_impl.hpp"

#include <algorithm>
#include <utility>
#include <vector>

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

// Receives the log reader's reports while a WAL is replayed. Every report is
// logged. Unless the database was opened to salvage, the first one also fails
// recovery -- a damaged WAL must never quietly lose acknowledged writes.
class RecoveryReporter final : public log::Reader::Reporter {
 public:
  RecoveryReporter(Logger* info_log, std::string file, bool ignore_corruption)
      : info_log_(info_log), file_(std::move(file)), ignore_corruption_(ignore_corruption) {}

  void Corruption(std::size_t bytes_dropped, const Status& reason) override {
    Log(info_log_, LogLevel::kWarn, "{}: dropping {} bytes: {}", file_, bytes_dropped,
        reason.ToString());
    if ((!ignore_corruption_ || reason.IsIoError()) && status_.ok()) {
      status_ = InFile(reason, file_);
    }
  }

  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  Logger* info_log_;
  std::string file_;
  bool ignore_corruption_;
  Status status_;
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

  // The lock comes first: nothing else in the directory may be touched --
  // not even the info log -- while another process has the database open.
  STRATA_ASSIGN_OR_RETURN(db_lock_, env_->LockFile(LockFileName(dbname_)));

  if (info_log_ == nullptr) {
    // Keep the previous session's log for diagnosis; it may explain a crash.
    static_cast<void>(env_->RenameFile(InfoLogFileName(dbname_), OldInfoLogFileName(dbname_)));
    STRATA_ASSIGN_OR_RETURN(owned_info_log_, NewFileLogger(env_, InfoLogFileName(dbname_)));
    info_log_ = owned_info_log_.get();
  }

  STRATA_ASSIGN_OR_RETURN(const std::vector<std::string> children, env_->GetChildren(dbname_));
  std::vector<std::uint64_t> logs;
  bool has_data = false;
  for (const std::string& child : children) {
    std::uint64_t number = 0;
    FileType type = FileType::kLockFile;
    if (!ParseFileName(child, &number, &type)) {
      continue;
    }
    next_file_number_ = std::max(next_file_number_, number + 1);
    if (type == FileType::kLogFile) {
      logs.push_back(number);
    }
    has_data = has_data || type == FileType::kLogFile || type == FileType::kTableFile ||
               type == FileType::kDescriptorFile || type == FileType::kCurrentFile;
  }
  if (options_.error_if_exists && has_data) {
    return Status::AlreadyExists(dbname_ + " already contains a database (error_if_exists)");
  }

  // Logs are numbered in creation order, so replaying them in numeric order
  // re-applies every write in the order it was acknowledged.
  std::sort(logs.begin(), logs.end());
  mem_ = std::make_unique<MemTable>();
  SequenceNumber max_sequence = 0;
  for (const std::uint64_t number : logs) {
    STRATA_RETURN_IF_ERROR(RecoverLogFile(number, &max_sequence));
  }
  last_sequence_ = max_sequence;

  STRATA_RETURN_IF_ERROR(NewLogFile());
  Log(info_log_, LogLevel::kInfo, "opened {}: replayed {} log file(s), last sequence {}", dbname_,
      logs.size(), last_sequence_);
  return Status::Ok();
}

Status DBImpl::RecoverLogFile(std::uint64_t number, SequenceNumber* max_sequence) {
  const std::string file_name = LogFileName(dbname_, number);
  STRATA_ASSIGN_OR_RETURN(auto file, env_->NewSequentialFile(file_name));
  RecoveryReporter reporter(info_log_, file_name, options_.ignore_wal_corruption);
  log::Reader reader(file.get(), &reporter);

  Slice record;
  std::string scratch;
  WriteBatch batch;
  std::uint64_t records = 0;
  while (reporter.status().ok() && reader.ReadRecord(&record, &scratch)) {
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
    STRATA_RETURN_IF_ERROR(InsertInto(batch, mem_.get()));
    if (batch.Count() > 0) {
      *max_sequence =
          std::max(*max_sequence, WriteBatchInternal::Sequence(batch) + batch.Count() - 1);
    }
    ++records;
  }
  STRATA_RETURN_IF_ERROR(reporter.status());
  Log(info_log_, LogLevel::kInfo, "recovered {} batch(es) from {}", records, file_name);
  return Status::Ok();
}

Status DBImpl::NewLogFile() {
  const std::uint64_t number = next_file_number_++;
  const std::string file_name = LogFileName(dbname_, number);
  STRATA_ASSIGN_OR_RETURN(auto file, env_->NewWritableFile(file_name));
  // Make the new log's *name* durable before any write relies on it: syncing
  // the records alone would leave a log that vanishes on power loss along with
  // every acknowledged write in it (see LogPowerLoss tests).
  STRATA_RETURN_IF_ERROR(env_->SyncDir(dbname_));
  log_ = nullptr;
  log_file_ = std::move(file);
  log_ = std::make_unique<log::Writer>(log_file_.get());
  log_number_ = number;
  return Status::Ok();
}

Status DBImpl::Write(const WriteOptions& options, const WriteBatch& batch) {
  if (batch.oversized()) {
    return Status::InvalidArgument("key or value exceeds the size limit (see WriteBatch)");
  }
  if (batch.Count() == 0) {
    return Status::Ok();
  }

  const std::lock_guard<std::mutex> guard(mutex_);
  if (!background_error_.ok()) {
    return background_error_;
  }

  // Stamp a copy with its sequence number; the caller's batch stays untouched
  // and can be reused.
  WriteBatch sequenced = batch;
  WriteBatchInternal::SetSequence(&sequenced, last_sequence_ + 1);

  // The durability boundary: once the record is in the log (and synced, if
  // asked), the write is acknowledged. Only then is it made visible.
  Status status = log_->AddRecord(WriteBatchInternal::Contents(sequenced));
  if (status.ok() && options.sync) {
    status = log_file_->Sync();
  }
  if (!status.ok()) {
    background_error_ = status;
    Log(info_log_, LogLevel::kError, "write-ahead log {} failed; refusing further writes: {}",
        log_number_, status.ToString());
    return status;
  }

  STRATA_RETURN_IF_ERROR(InsertInto(sequenced, mem_.get()));
  last_sequence_ += sequenced.Count();
  return Status::Ok();
}

Result<std::string> DBImpl::Get(const ReadOptions& /*options*/, Slice key) {
  if (key.size() > WriteBatch::kMaxKeySize) {
    return Status::InvalidArgument("key exceeds the size limit");
  }
  const std::lock_guard<std::mutex> guard(mutex_);
  const LookupKey lookup(key, last_sequence_);
  std::string value;
  if (mem_->Get(lookup, &value) == MemTable::LookupResult::kFound) {
    return value;
  }
  return Status::NotFound();
}

Status DBImpl::Close() {
  const std::lock_guard<std::mutex> guard(mutex_);
  Status status;
  if (log_file_ != nullptr) {
    log_ = nullptr;
    status = log_file_->Close();
    log_file_ = nullptr;
    Log(info_log_, LogLevel::kInfo, "closed {}", dbname_);
  }
  return status;
}

}  // namespace strata

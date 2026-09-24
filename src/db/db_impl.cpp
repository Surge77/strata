#include "db/db_impl.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "db/filename.hpp"
#include "db/write_batch_internal.hpp"
#include "log/log_reader.hpp"

namespace strata {
namespace {

// Target size of the batches consolidation writes: large enough that the log
// is not all headers, small enough that no single record is huge.
constexpr std::size_t kConsolidationBatchBytes = 1U << 20U;

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
// logged; whether any of them is fatal is decided after the whole log has been
// read (see RecoverLogFile), because that depends on what follows the damage.
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

// One recovered memtable entry, for consolidation.
struct Entry {
  SequenceNumber sequence;
  ValueType type;
  std::string key;
  std::string value;
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

  // The lock comes first: nothing else in the directory may be touched while
  // another process has the database open.
  STRATA_ASSIGN_OR_RETURN(db_lock_, env_->LockFile(LockFileName(dbname_)));

  STRATA_ASSIGN_OR_RETURN(const std::vector<std::string> children, env_->GetChildren(dbname_));
  std::vector<std::uint64_t> logs;
  bool has_data = false;
  for (const std::string& child : children) {
    std::uint64_t number = 0;
    FileType type = FileType::kLockFile;
    if (!ParseFileName(child, &number, &type)) {
      continue;
    }
    if (number == UINT64_MAX) {
      // The next file would need number UINT64_MAX + 1, which wraps to 0 and
      // would sort -- and replay -- before everything else.
      return Status::Corruption(dbname_ + "/" + child + ": file number space exhausted");
    }
    next_file_number_ = std::max(next_file_number_, number + 1);
    if (type == FileType::kLogFile) {
      logs.push_back(number);
    }
    has_data = has_data || type == FileType::kLogFile || type == FileType::kTableFile ||
               type == FileType::kDescriptorFile || type == FileType::kCurrentFile;
  }
  // Checked before anything is written, so rejecting an existing database
  // leaves it exactly as it was.
  if (options_.error_if_exists && has_data) {
    return Status::AlreadyExists(dbname_ + " already contains a database (error_if_exists)");
  }

  if (info_log_ == nullptr) {
    // Appended to, never rotated: rotating on every open would push the log of
    // a crashed session out after two failed attempts to reopen -- exactly
    // when it is needed.
    STRATA_ASSIGN_OR_RETURN(owned_info_log_, NewFileLogger(env_, InfoLogFileName(dbname_)));
    info_log_ = owned_info_log_.get();
  }
  Log(info_log_, LogLevel::kInfo, "=== opening {} ===", dbname_);

  // Logs are numbered in creation order, so replaying them in numeric order
  // re-applies every write in the order it was acknowledged.
  std::sort(logs.begin(), logs.end());
  mem_ = std::make_unique<MemTable>();
  SequenceNumber max_sequence = 0;
  for (std::size_t index = 0; index < logs.size(); ++index) {
    const bool newest = index + 1 == logs.size();
    STRATA_RETURN_IF_ERROR(RecoverLogFile(logs[index], newest, &max_sequence));
  }
  last_sequence_ = max_sequence;

  STRATA_RETURN_IF_ERROR(NewLogFile());
  STRATA_RETURN_IF_ERROR(ConsolidateRecoveredLogs(logs));
  Log(info_log_, LogLevel::kInfo, "opened {}: recovered {} log file(s), last sequence {}", dbname_,
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
  std::uint64_t duplicates = 0;
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
      // Already applied from an older log: a copy written by a consolidation
      // that crashed before it could delete the logs it copied.
      ++duplicates;
      continue;
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
      // zeros. Nothing in it was acknowledged as durable (every synced write
      // precedes it), so it is treated like any torn tail.
      Log(info_log_, LogLevel::kWarn,
          "{}: treating damage at the end of the newest log as a torn write", file_name);
    } else if (!options_.ignore_wal_corruption) {
      return reporter.first_problem();
    }
  }
  Log(info_log_, LogLevel::kInfo, "{}: {} batch(es) applied, {} duplicate(s) skipped", file_name,
      applied, duplicates);
  return Status::Ok();
}

Status DBImpl::ConsolidateRecoveredLogs(const std::vector<std::uint64_t>& old_logs) {
  if (old_logs.empty()) {
    return Status::Ok();
  }

  // Everything recovered goes into the new log, ordered by sequence number and
  // grouped into batches of consecutive sequences, then synced. Only after that
  // are the replayed logs deleted. This is what makes recovery leave the
  // database in a clean state:
  //   * logs replayed from the page cache after a crash can no longer lose
  //     their unsynced tail underneath later synced writes (they are gone);
  //   * damage dropped by salvage or torn-tail handling does not reappear on
  //     the next open with sequence numbers the new writes have reused;
  //   * the number of logs stays at one instead of growing with every open.
  // A crash in the middle leaves old logs *and* a partial copy; replay skips
  // the copy's batches as duplicates (RecoverLogFile).
  std::vector<Entry> entries;
  auto it = mem_->NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ParsedInternalKey parsed;
    if (!ParseInternalKey(it->key(), &parsed)) {
      return Status::Internal("memtable holds an unparseable internal key");
    }
    entries.push_back(
        {parsed.sequence, parsed.type, std::string(parsed.user_key), std::string(it->value())});
  }
  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.sequence < b.sequence; });

  WriteBatch batch;
  SequenceNumber next_in_batch = 0;
  auto flush_batch = [&]() -> Status {
    if (batch.Count() == 0) {
      return Status::Ok();
    }
    STRATA_RETURN_IF_ERROR(log_->AddRecord(WriteBatchInternal::Contents(batch)));
    batch.Clear();
    return Status::Ok();
  };
  for (const Entry& entry : entries) {
    if (batch.Count() > 0 &&
        (entry.sequence != next_in_batch || batch.ApproximateSize() > kConsolidationBatchBytes)) {
      STRATA_RETURN_IF_ERROR(flush_batch());
    }
    if (batch.Count() == 0) {
      WriteBatchInternal::SetSequence(&batch, entry.sequence);
    }
    if (entry.type == ValueType::kValue) {
      batch.Put(entry.key, entry.value);
    } else {
      batch.Delete(entry.key);
    }
    next_in_batch = entry.sequence + 1;
  }
  STRATA_RETURN_IF_ERROR(flush_batch());
  STRATA_RETURN_IF_ERROR(log_file_->Sync());

  for (const std::uint64_t number : old_logs) {
    STRATA_RETURN_IF_ERROR(env_->RemoveFile(LogFileName(dbname_, number)));
  }
  STRATA_RETURN_IF_ERROR(env_->SyncDir(dbname_));
  Log(info_log_, LogLevel::kInfo, "consolidated {} entrie(s) from {} log(s) into log {}",
      entries.size(), old_logs.size(), log_number_);
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
  if (closed_) {
    return Status::InvalidArgument("database is closed");
  }
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
  if (status.ok()) {
    status = InsertInto(sequenced, mem_.get());
  }
  if (!status.ok()) {
    // Either the log may now hold a partial record, or the memtable holds part
    // of a batch whose sequence numbers were not consumed. Both would be
    // compounded by accepting more writes.
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
  const std::lock_guard<std::mutex> guard(mutex_);
  if (closed_) {
    return Status::InvalidArgument("database is closed");
  }
  const LookupKey lookup(key, last_sequence_);
  std::string value;
  if (mem_->Get(lookup, &value) == MemTable::LookupResult::kFound) {
    return value;
  }
  return Status::NotFound();
}

Status DBImpl::Close() {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (closed_) {
    return Status::Ok();
  }
  closed_ = true;
  Status status;
  if (log_file_ != nullptr) {
    // A clean close leaves every acknowledged write durable, whatever its
    // WriteOptions: one fdatasync, paid once per session.
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

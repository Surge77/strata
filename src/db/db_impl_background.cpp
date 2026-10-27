// DBImpl's background work: flushing the immutable memtable, compacting
// levels, and the write stalls that keep level 0 from outgrowing compaction.

#include <chrono>
#include <thread>

#include "db/db_impl.hpp"
#include "db/filename.hpp"

namespace strata {
namespace {

// A write that finds level 0 long waits this long, once. Spreading the stall
// over many writes keeps latency smooth instead of stopping one for seconds.
constexpr auto kSlowdownDelay = std::chrono::milliseconds(1);

}  // namespace

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>* lock, bool force) {
  bool allow_delay = !force;
  while (true) {
    // Checked on every pass: Close can happen while this write waits below.
    if (closed_) {
      return Status::InvalidArgument("database is closed");
    }
    if (!background_error_.ok()) {
      return background_error_;
    }
    const std::size_t level0 = versions_->current()->files(0).size();
    if (allow_delay && level0 >= kL0SlowdownWritesTrigger) {
      ++stats_.write_slowdowns;
      lock->unlock();
      std::this_thread::sleep_for(kSlowdownDelay);
      lock->lock();
      allow_delay = false;
      continue;
    }
    // An empty memtable always takes the write: its fixed overhead can exceed
    // a tiny write_buffer_size, and retiring it would flush nothing and then
    // retire the next empty one, forever.
    if (mem_->empty() || (!force && mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
      return Status::Ok();
    }
    if (imm_ != nullptr) {
      background_done_.wait(*lock);  // the previous memtable is still flushing
      continue;
    }
    if (level0 >= kL0StopWritesTrigger) {
      ++stats_.write_stops;
      background_done_.wait(*lock);  // until compaction shortens level 0
      continue;
    }
    STRATA_RETURN_IF_ERROR(SwitchMemTable(lock));
    force = false;  // the fresh memtable is empty, so the next pass returns
  }
}

Status DBImpl::SwitchMemTable(std::unique_lock<std::mutex>* lock) {
  if (Status status = NewLogFile(lock); !status.ok()) {
    background_error_ = status;
    background_done_.notify_all();  // waiters re-check background_error_
    return status;
  }
  imm_ = std::move(mem_);
  imm_last_sequence_ = last_sequence_;
  mem_ = std::make_shared<MemTable>(&internal_comparator_);
  background_work_.notify_one();
  return Status::Ok();
}

bool DBImpl::NeedsCompaction() const {
  const auto version = versions_->current();
  for (int level = 0; level + 1 < kNumLevels; ++level) {
    if (CompactionScore(*version, level, options_.max_bytes_for_level_base) >= 1.0) {
      return true;
    }
  }
  return false;
}

bool DBImpl::HasBackgroundWork() const {
  return (imm_ != nullptr && !flushes_paused_) || manual_compaction_level_.has_value() ||
         (!compactions_paused_ && NeedsCompaction());
}

SequenceNumber DBImpl::SmallestSnapshot() const {
  return snapshots_.empty() ? last_sequence_ : snapshots_.oldest();
}

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    background_work_.wait(
        lock, [this] { return shutting_down_ || (background_error_.ok() && HasBackgroundWork()); });
    if (shutting_down_) {
      return;
    }
    background_running_ = true;
    if (imm_ != nullptr && !flushes_paused_) {
      // Flush first: writes may be waiting for the memtable to drain.
      FlushImmutable(&lock);
    } else if (manual_compaction_level_.has_value()) {
      const int level = *manual_compaction_level_;
      manual_compaction_level_.reset();
      const auto compaction =
          PickLevelCompaction(*versions_->current(), internal_comparator_, level);
      manual_compaction_status_ =
          compaction.has_value() ? DoCompaction(*compaction, &lock) : Status::Ok();
      manual_compaction_done_ = true;
    } else if (const auto compaction =
                   PickCompaction(*versions_->current(), internal_comparator_,
                                  options_.max_bytes_for_level_base, &compact_next_start_)) {
      static_cast<void>(DoCompaction(*compaction, &lock));
    }
    background_running_ = false;
    background_done_.notify_all();
  }
}

void DBImpl::FlushImmutable(std::unique_lock<std::mutex>* lock) {
  const std::shared_ptr<MemTable> imm = imm_;
  VersionEdit edit;
  Status status = WriteLevel0Table(imm.get(), &edit, lock);
  if (status.ok()) {
    // Logs before the active one hold only what `imm` held, which is now in
    // a table: committing the active log number retires them.
    edit.SetLogNumber(log_number_);
    // Not last_sequence_: writes that arrived during the flush are only in
    // the active log, and replay skips anything at or below this number.
    versions_->SetLastSequence(imm_last_sequence_);
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
}

Status DBImpl::DoCompaction(const Compaction& compaction, std::unique_lock<std::mutex>* lock) {
  VersionEdit edit;
  for (std::size_t which = 0; which < compaction.inputs.size(); ++which) {
    for (const FileMetaData& file : compaction.inputs[which]) {
      edit.RemoveFile(compaction.level + static_cast<int>(which), file.number);
    }
  }

  Status status;
  if (compaction.IsTrivialMove()) {
    edit.AddFile(compaction.output_level(), compaction.inputs[0].front());
    status = versions_->LogAndApply(&edit);
    if (status.ok()) {
      ++stats_.trivial_moves;
      Log(info_log_, LogLevel::kInfo, "moved table {} to level {}",
          compaction.inputs[0].front().number, compaction.output_level());
    }
  } else {
    const std::shared_ptr<const Version> version = versions_->current();
    std::vector<std::uint64_t> allocated;
    const CompactionJobOptions job{
        .dbname = dbname_,
        .env = env_,
        .comparator = &internal_comparator_,
        .tables = table_cache_.get(),
        .use_filter = options_.bloom_filter,
        .max_file_size = options_.max_file_size,
        .smallest_snapshot = SmallestSnapshot(),
        .new_file_number =
            [this, &allocated] {
              const std::lock_guard<std::mutex> guard(mutex_);
              const std::uint64_t number = versions_->NewFileNumber();
              pending_outputs_.insert(number);
              allocated.push_back(number);
              return number;
            },
    };
    std::vector<FileMetaData> outputs;
    CompactionStats job_stats;
    lock->unlock();
    status = RunCompaction(job, compaction, *version, &outputs, &job_stats);
    lock->lock();
    if (status.ok()) {
      for (FileMetaData& output : outputs) {
        edit.AddFile(compaction.output_level(), std::move(output));
      }
      status = versions_->LogAndApply(&edit);
      if (!status.ok()) {
        // Written but never committed: no version will ever reference them.
        for (const auto& [level, output] : edit.new_files()) {
          table_cache_->Evict(output.number);
          static_cast<void>(env_->RemoveFile(TableFileName(dbname_, output.number)));
        }
      }
    }
    for (const std::uint64_t number : allocated) {
      pending_outputs_.erase(number);
    }
    if (status.ok()) {
      ++stats_.compactions;
      stats_.compaction_bytes_read += job_stats.bytes_read;
      stats_.compaction_bytes_written += job_stats.bytes_written;
      Log(info_log_, LogLevel::kInfo, "compacted level {} into {}: {} bytes read, {} written",
          compaction.level, compaction.output_level(), job_stats.bytes_read,
          job_stats.bytes_written);
    }
  }

  if (!status.ok()) {
    background_error_ = status;
    Log(info_log_, LogLevel::kError, "compaction failed; refusing further writes: {}",
        status.ToString());
    return status;
  }
  DeleteObsoleteFiles();
  return Status::Ok();
}

DBStats DBImpl::GetStats() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return stats_;
}

Status DBImpl::CompactLevelForTest(int level) {
  std::unique_lock<std::mutex> lock(mutex_);
  STRATA_RETURN_IF_ERROR(background_error_);
  manual_compaction_level_ = level;
  manual_compaction_done_ = false;
  background_work_.notify_one();
  background_done_.wait(lock, [this] {
    return manual_compaction_done_ || !background_error_.ok() || shutting_down_;
  });
  return manual_compaction_done_ ? manual_compaction_status_ : background_error_;
}

void DBImpl::WaitForCompactionsForTest() {
  std::unique_lock<std::mutex> lock(mutex_);
  background_done_.wait(lock, [this] {
    return (!background_running_ && !HasBackgroundWork()) || !background_error_.ok() ||
           shutting_down_;
  });
}

void DBImpl::PauseCompactionsForTest(bool paused) {
  const std::lock_guard<std::mutex> guard(mutex_);
  compactions_paused_ = paused;
  background_work_.notify_all();
}

void DBImpl::PauseFlushesForTest(bool paused) {
  const std::lock_guard<std::mutex> guard(mutex_);
  flushes_paused_ = paused;
  background_work_.notify_all();
}

void DBImpl::SetAfterMemTableInsertHookForTest(std::function<void()> hook) {
  after_memtable_insert_hook_ = std::move(hook);
}

bool DBImpl::HasImmutableMemTableForTest() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return imm_ != nullptr;
}

std::vector<FileMetaData> DBImpl::FilesAtLevelForTest(int level) {
  const std::lock_guard<std::mutex> guard(mutex_);
  return versions_->current()->files(level);
}

std::uint64_t DBImpl::LevelBytesForTest(int level) {
  const std::lock_guard<std::mutex> guard(mutex_);
  return versions_->current()->LevelBytes(level);
}

void DBImpl::EvictTableForTest(std::uint64_t number) {
  table_cache_->Evict(number);
}

}  // namespace strata

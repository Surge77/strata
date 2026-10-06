#include "support/fault_injection_env.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace strata::test {
namespace {

std::string DirName(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Forwards to a real file and tells the env what was appended and synced.
class FaultWritableFile final : public WritableFile {
 public:
  FaultWritableFile(FaultInjectionEnv* env, std::string path, std::unique_ptr<WritableFile> base)
      : env_(env), path_(std::move(path)), base_(std::move(base)) {}

  Status Append(Slice data) override {
    STRATA_RETURN_IF_ERROR(env_->CheckWritable());
    const std::uint64_t allowed = env_->AppendAllowance(path_, data.size());
    if (allowed < data.size()) {
      if (allowed > 0) {
        const Slice fits = data.substr(0, static_cast<std::size_t>(allowed));
        STRATA_RETURN_IF_ERROR(base_->Append(fits));
        env_->RecordAppend(path_, fits.size());
      }
      return Status::IoError(path_ + ": No space left on device (injected)");
    }
    STRATA_RETURN_IF_ERROR(base_->Append(data));
    env_->RecordAppend(path_, data.size());
    return Status::Ok();
  }

  Status Flush() override {
    STRATA_RETURN_IF_ERROR(env_->CheckWritable());
    return base_->Flush();
  }

  Status Sync() override {
    STRATA_RETURN_IF_ERROR(env_->CheckWritable());
    STRATA_RETURN_IF_ERROR(env_->InjectedSyncFailure(path_));
    STRATA_RETURN_IF_ERROR(base_->Sync());
    return env_->RecordSync(path_);
  }

  Status Close() override { return base_->Close(); }

 private:
  FaultInjectionEnv* env_;
  std::string path_;
  std::unique_ptr<WritableFile> base_;
};

// Forwards reads to a real file unless the env is injecting read errors.
class FaultRandomAccessFile final : public RandomAccessFile {
 public:
  FaultRandomAccessFile(FaultInjectionEnv* env, std::string path,
                        std::unique_ptr<RandomAccessFile> base)
      : env_(env), path_(std::move(path)), base_(std::move(base)) {}

  Result<Slice> Read(std::uint64_t offset, std::size_t n, char* scratch) const override {
    STRATA_RETURN_IF_ERROR(env_->InjectedReadFailure(path_));
    return base_->Read(offset, n, scratch);
  }

 private:
  FaultInjectionEnv* env_;
  std::string path_;
  std::unique_ptr<RandomAccessFile> base_;
};

class FaultSequentialFile final : public SequentialFile {
 public:
  FaultSequentialFile(FaultInjectionEnv* env, std::string path,
                      std::unique_ptr<SequentialFile> base)
      : env_(env), path_(std::move(path)), base_(std::move(base)) {}

  Result<Slice> Read(std::size_t n, char* scratch) override {
    STRATA_RETURN_IF_ERROR(env_->InjectedReadFailure(path_));
    return base_->Read(n, scratch);
  }
  Status Skip(std::uint64_t n) override { return base_->Skip(n); }

 private:
  FaultInjectionEnv* env_;
  std::string path_;
  std::unique_ptr<SequentialFile> base_;
};

}  // namespace

FaultInjectionEnv::FaultInjectionEnv(Env* base) : EnvWrapper(base) {}

Result<std::unique_ptr<RandomAccessFile>> FaultInjectionEnv::NewRandomAccessFile(
    const std::string& path) {
  STRATA_ASSIGN_OR_RETURN(auto file, target()->NewRandomAccessFile(path));
  return std::unique_ptr<RandomAccessFile>(
      std::make_unique<FaultRandomAccessFile>(this, path, std::move(file)));
}

Result<std::unique_ptr<SequentialFile>> FaultInjectionEnv::NewSequentialFile(
    const std::string& path) {
  STRATA_ASSIGN_OR_RETURN(auto file, target()->NewSequentialFile(path));
  return std::unique_ptr<SequentialFile>(
      std::make_unique<FaultSequentialFile>(this, path, std::move(file)));
}

void FaultInjectionEnv::SetDiskFull(std::string path_pattern, std::uint64_t bytes_left) {
  const std::lock_guard<std::mutex> guard(mutex_);
  disk_full_ = DiskFull{.path_pattern = std::move(path_pattern), .bytes_left = bytes_left};
}

void FaultInjectionEnv::ClearDiskFull() {
  const std::lock_guard<std::mutex> guard(mutex_);
  disk_full_.reset();
}

std::uint64_t FaultInjectionEnv::AppendAllowance(const std::string& path, std::uint64_t bytes) {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (!disk_full_ || path.find(disk_full_->path_pattern) == std::string::npos) {
    return bytes;
  }
  const std::uint64_t allowed = std::min(bytes, disk_full_->bytes_left);
  disk_full_->bytes_left -= allowed;
  return allowed;
}

void FaultInjectionEnv::SetReadFailure(std::optional<std::string> path_pattern,
                                       std::uint64_t after_reads) {
  const std::lock_guard<std::mutex> guard(mutex_);
  read_failure_.reset();
  if (path_pattern) {
    read_failure_ =
        ReadFailure{.path_pattern = std::move(*path_pattern), .reads_left = after_reads};
  }
}

Status FaultInjectionEnv::InjectedReadFailure(const std::string& path) {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (!read_failure_ || path.find(read_failure_->path_pattern) == std::string::npos) {
    return Status::Ok();
  }
  if (read_failure_->reads_left > 0) {
    --read_failure_->reads_left;
    return Status::Ok();
  }
  return Status::IoError(path + ": injected read error (EIO)");
}

Status FaultInjectionEnv::CheckWritable() {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (active_ && ops_until_crash_) {
    if (*ops_until_crash_ == 0) {
      active_ = false;
    } else {
      --*ops_until_crash_;
    }
  }
  if (!active_) {
    return Status::IoError("filesystem inactive (simulated crash)");
  }
  ++mutating_ops_;
  return Status::Ok();
}

void FaultInjectionEnv::SetCrashAfterOps(std::optional<std::uint64_t> ops) {
  const std::lock_guard<std::mutex> guard(mutex_);
  ops_until_crash_ = ops;
}

std::uint64_t FaultInjectionEnv::mutating_ops() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return mutating_ops_;
}

Status FaultInjectionEnv::InjectedSyncFailure(const std::string& path) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const bool matches = path.find(sync_failure_pattern_) != std::string::npos;
  return fail_syncs_ && matches ? Status::IoError(path + ": injected sync failure") : Status::Ok();
}

void FaultInjectionEnv::RecordAppend(const std::string& path, std::uint64_t bytes) {
  const std::lock_guard<std::mutex> guard(mutex_);
  files_[path].size += bytes;
}

Status FaultInjectionEnv::RecordSync(const std::string& path) {
  const std::lock_guard<std::mutex> guard(mutex_);
  FileState& state = files_[path];
  state.synced_size = state.size;
  ++sync_count_;
  return Status::Ok();
}

void FaultInjectionEnv::SetFilesystemActive(bool active) {
  const std::lock_guard<std::mutex> guard(mutex_);
  active_ = active;
}

void FaultInjectionEnv::SetSyncFailure(bool fail, std::string path_pattern) {
  const std::lock_guard<std::mutex> guard(mutex_);
  fail_syncs_ = fail;
  sync_failure_pattern_ = std::move(path_pattern);
}

std::uint64_t FaultInjectionEnv::sync_count() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return sync_count_;
}

std::uint64_t FaultInjectionEnv::sync_dir_count() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return sync_dir_count_;
}

Result<std::unique_ptr<WritableFile>> FaultInjectionEnv::OpenTracked(const std::string& path,
                                                                     bool append) {
  STRATA_RETURN_IF_ERROR(CheckWritable());
  const bool existed = target()->FileExists(path);
  std::uint64_t existing_size = 0;
  if (append && existed) {
    STRATA_ASSIGN_OR_RETURN(existing_size, target()->GetFileSize(path));
  }

  Result<std::unique_ptr<WritableFile>> opened =
      append ? target()->NewAppendableFile(path) : target()->NewWritableFile(path);
  if (!opened.ok()) {
    return opened.status();
  }

  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (!existed) {
      pending_dir_ops_.push_back({.kind = DirOp::Kind::kCreate, .path = path});
      files_[path] = FileState{};
    } else if (append) {
      // Keep what is already known about a tracked file; an untracked one
      // predates this env and is durable in full.
      if (files_.find(path) == files_.end()) {
        files_[path] = FileState{.size = existing_size, .synced_size = existing_size};
      }
    } else {
      // Truncation: the old contents are gone and nothing new is synced yet.
      files_[path] = FileState{};
    }
  }
  return std::unique_ptr<WritableFile>(
      std::make_unique<FaultWritableFile>(this, path, std::move(opened).value()));
}

Result<std::unique_ptr<WritableFile>> FaultInjectionEnv::NewWritableFile(const std::string& path) {
  return OpenTracked(path, /*append=*/false);
}

Result<std::unique_ptr<WritableFile>> FaultInjectionEnv::NewAppendableFile(
    const std::string& path) {
  return OpenTracked(path, /*append=*/true);
}

std::string FaultInjectionEnv::DurableContents(const std::string& path) {
  auto file = target()->NewSequentialFile(path);
  if (!file.ok()) {
    return {};
  }
  std::uint64_t durable = UINT64_MAX;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto found = files_.find(path);
    if (found != files_.end()) {
      durable = found->second.synced_size;
    }
  }
  std::string contents;
  std::string scratch(64 * 1024, '\0');
  while (contents.size() < durable) {
    auto chunk = (*file)->Read(scratch.size(), scratch.data());
    if (!chunk.ok() || chunk->empty()) {
      break;
    }
    contents.append(*chunk);
  }
  if (contents.size() > durable) {
    contents.resize(durable);
  }
  return contents;
}

Status FaultInjectionEnv::RemoveFile(const std::string& path) {
  STRATA_RETURN_IF_ERROR(CheckWritable());
  std::string saved = DurableContents(path);
  STRATA_RETURN_IF_ERROR(target()->RemoveFile(path));
  const std::lock_guard<std::mutex> guard(mutex_);
  files_.erase(path);
  pending_dir_ops_.push_back(
      {.kind = DirOp::Kind::kRemove, .path = path, .saved_bytes = std::move(saved)});
  return Status::Ok();
}

Status FaultInjectionEnv::RenameFile(const std::string& from, const std::string& to) {
  STRATA_RETURN_IF_ERROR(CheckWritable());
  const bool had_target = target()->FileExists(to);
  std::string saved = had_target ? DurableContents(to) : std::string();
  std::uint64_t source_size = 0;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (files_.find(from) == files_.end()) {
      // An untracked source predates this env, so all of it is durable.
      auto size = target()->GetFileSize(from);
      source_size = size.ok() ? *size : 0;
    }
  }
  STRATA_RETURN_IF_ERROR(target()->RenameFile(from, to));

  const std::lock_guard<std::mutex> guard(mutex_);
  const auto found = files_.find(from);
  const FileState moved = found != files_.end()
                              ? found->second
                              : FileState{.size = source_size, .synced_size = source_size};
  if (found != files_.end()) {
    files_.erase(found);
  }
  files_[to] = moved;
  pending_dir_ops_.push_back({.kind = DirOp::Kind::kRename,
                              .path = from,
                              .target = to,
                              .had_target = had_target,
                              .saved_bytes = std::move(saved)});
  return Status::Ok();
}

Status FaultInjectionEnv::SyncDir(const std::string& dir) {
  STRATA_RETURN_IF_ERROR(CheckWritable());
  STRATA_RETURN_IF_ERROR(InjectedSyncFailure(dir));
  STRATA_RETURN_IF_ERROR(target()->SyncDir(dir));

  const std::lock_guard<std::mutex> guard(mutex_);
  ++sync_dir_count_;
  // Every pending change to this directory's entries is now durable.
  std::erase_if(pending_dir_ops_, [&dir](const DirOp& op) {
    const std::string& changed = op.kind == DirOp::Kind::kRename ? op.target : op.path;
    return DirName(changed) == dir;
  });
  return Status::Ok();
}

Status FaultInjectionEnv::UndoDirOp(const DirOp& op) {
  auto rewrite = [this](const std::string& path, const std::string& bytes) -> Status {
    STRATA_ASSIGN_OR_RETURN(auto file, target()->NewWritableFile(path));
    STRATA_RETURN_IF_ERROR(file->Append(bytes));
    STRATA_RETURN_IF_ERROR(file->Close());
    files_[path] = FileState{.size = bytes.size(), .synced_size = bytes.size()};
    return Status::Ok();
  };

  switch (op.kind) {
    case DirOp::Kind::kCreate:
      if (target()->FileExists(op.path)) {
        STRATA_RETURN_IF_ERROR(target()->RemoveFile(op.path));
      }
      files_.erase(op.path);
      return Status::Ok();

    case DirOp::Kind::kRemove:
      return rewrite(op.path, op.saved_bytes);

    case DirOp::Kind::kRename: {
      if (target()->FileExists(op.target)) {
        STRATA_RETURN_IF_ERROR(target()->RenameFile(op.target, op.path));
        const auto found = files_.find(op.target);
        if (found != files_.end()) {
          files_[op.path] = found->second;
          files_.erase(op.target);
        }
      }
      if (op.had_target) {
        return rewrite(op.target, op.saved_bytes);
      }
      return Status::Ok();
    }
  }
  return Status::Internal("unknown directory operation");
}

Status FaultInjectionEnv::SimulatePowerLoss() {
  const std::lock_guard<std::mutex> guard(mutex_);

  // Directory changes first, newest first, so each undo sees the namespace as
  // it was right after the change it reverses.
  for (auto op = pending_dir_ops_.rbegin(); op != pending_dir_ops_.rend(); ++op) {
    STRATA_RETURN_IF_ERROR(UndoDirOp(*op));
  }
  pending_dir_ops_.clear();

  // Then contents: every surviving file is cut back to its last sync.
  for (auto& [path, state] : files_) {
    if (state.size > state.synced_size) {
      std::error_code error;
      std::filesystem::resize_file(path, state.synced_size, error);
      if (error && error != std::errc::no_such_file_or_directory) {
        return Status::IoError(path + ": " + error.message());
      }
      state.size = state.synced_size;
    }
  }
  return Status::Ok();
}

}  // namespace strata::test

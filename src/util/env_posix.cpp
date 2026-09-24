// The POSIX implementation of Env: the only file in the engine that makes
// filesystem system calls.

#include "strata/env.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace strata {
namespace {

// Converts the current errno into a Status naming the path involved. ENOENT
// becomes NotFound so callers can tell "absent" from "broken".
Status ErrnoStatus(const std::string& context, int error_number) {
  const std::string message = context + ": " + std::strerror(error_number);
  if (error_number == ENOENT) {
    return Status::NotFound(message);
  }
  return Status::IoError(message);
}

// Closes a descriptor, retrying nothing: on Linux the descriptor is released
// even when close() reports EINTR, so retrying could close an unrelated
// descriptor another thread has just been given.
int CloseDescriptor(int fd) noexcept {
  return ::close(fd);
}

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  ~PosixSequentialFile() override { static_cast<void>(CloseDescriptor(fd_)); }
  PosixSequentialFile(const PosixSequentialFile&) = delete;
  PosixSequentialFile& operator=(const PosixSequentialFile&) = delete;
  PosixSequentialFile(PosixSequentialFile&&) = delete;
  PosixSequentialFile& operator=(PosixSequentialFile&&) = delete;

  Result<Slice> Read(std::size_t n, char* scratch) override {
    std::size_t total = 0;
    // Loop until `n` bytes or end of file: read() may legally return fewer
    // bytes than asked even mid-file, and callers treat a short result as EOF.
    while (total < n) {
      const ssize_t got = ::read(fd_, scratch + total, n - total);
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        return ErrnoStatus(path_, errno);
      }
      if (got == 0) {
        break;
      }
      total += static_cast<std::size_t>(got);
    }
    return Slice(scratch, total);
  }

  Status Skip(std::uint64_t n) override {
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) < 0) {
      return ErrnoStatus(path_, errno);
    }
    return Status::Ok();
  }

 private:
  std::string path_;
  int fd_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  ~PosixRandomAccessFile() override { static_cast<void>(CloseDescriptor(fd_)); }
  PosixRandomAccessFile(const PosixRandomAccessFile&) = delete;
  PosixRandomAccessFile& operator=(const PosixRandomAccessFile&) = delete;
  PosixRandomAccessFile(PosixRandomAccessFile&&) = delete;
  PosixRandomAccessFile& operator=(PosixRandomAccessFile&&) = delete;

  // pread() takes an explicit offset and does not move a shared file position,
  // which is what makes this safe to call from many threads at once.
  Result<Slice> Read(std::uint64_t offset, std::size_t n, char* scratch) const override {
    std::size_t total = 0;
    while (total < n) {
      const ssize_t got =
          ::pread(fd_, scratch + total, n - total, static_cast<off_t>(offset + total));
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        return ErrnoStatus(path_, errno);
      }
      if (got == 0) {
        break;
      }
      total += static_cast<std::size_t>(got);
    }
    return Slice(scratch, total);
  }

 private:
  std::string path_;
  int fd_;
};

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {
    buffer_.reserve(kBufferSize);
  }

  ~PosixWritableFile() override {
    if (fd_ >= 0) {
      static_cast<void>(Close());
    }
  }
  PosixWritableFile(const PosixWritableFile&) = delete;
  PosixWritableFile& operator=(const PosixWritableFile&) = delete;
  PosixWritableFile(PosixWritableFile&&) = delete;
  PosixWritableFile& operator=(PosixWritableFile&&) = delete;

  // Small appends -- a WAL record is often tens of bytes -- are collected in a
  // user-space buffer so that each one does not cost a system call. Large
  // appends bypass the buffer instead of being copied through it.
  Status Append(Slice data) override {
    STRATA_RETURN_IF_ERROR(error_);
    if (buffer_.size() + data.size() <= kBufferSize) {
      buffer_.append(data);
      return Status::Ok();
    }
    STRATA_RETURN_IF_ERROR(FlushBuffer());
    if (data.size() <= kBufferSize) {
      buffer_.append(data);
      return Status::Ok();
    }
    return Remember(WriteUnbuffered(data));
  }

  Status Flush() override {
    STRATA_RETURN_IF_ERROR(error_);
    return FlushBuffer();
  }

  // fdatasync rather than fsync: the engine's durability depends on file
  // contents and size, not on metadata such as modification time, and
  // fdatasync skips the extra metadata write.
  Status Sync() override {
    STRATA_RETURN_IF_ERROR(error_);
    STRATA_RETURN_IF_ERROR(FlushBuffer());
    if (::fdatasync(fd_) < 0) {
      return Remember(ErrnoStatus(path_, errno));
    }
    return Status::Ok();
  }

  Status Close() override {
    Status status = error_.ok() ? FlushBuffer() : error_;
    if (CloseDescriptor(fd_) < 0 && status.ok()) {
      status = ErrnoStatus(path_, errno);
    }
    fd_ = -1;
    return status;
  }

 private:
  static constexpr std::size_t kBufferSize = 64 * 1024;

  // Errors are sticky (see WritableFile). After a failed write it is unknown
  // how many bytes reached the file, and after a failed fdatasync Linux may
  // already have dropped the dirty pages, so a later "successful" Sync would
  // be a lie. Every call after the first failure returns that failure.
  Status Remember(Status status) {
    if (!status.ok() && error_.ok()) {
      error_ = status;
    }
    return status;
  }

  Status FlushBuffer() {
    Status status = WriteUnbuffered(buffer_);
    buffer_.clear();
    return Remember(std::move(status));
  }

  Status WriteUnbuffered(Slice data) {
    while (!data.empty()) {
      const ssize_t wrote = ::write(fd_, data.data(), data.size());
      if (wrote < 0) {
        if (errno == EINTR) {
          continue;
        }
        return ErrnoStatus(path_, errno);
      }
      if (wrote == 0) {
        // POSIX permits this for a non-empty buffer; retrying would spin.
        return Status::IoError(path_ + ": write made no progress");
      }
      data.remove_prefix(static_cast<std::size_t>(wrote));
    }
    return Status::Ok();
  }

  std::string path_;
  int fd_;
  std::string buffer_;
  Status error_;
};

// fcntl() record locks belong to a process, not a descriptor, which has two
// consequences this registry exists to contain:
//
//   1. A second lock on the same file from the same process silently succeeds,
//      so opening one database twice would let two writers corrupt it.
//   2. Closing *any* descriptor for a file releases *all* of the process's locks
//      on it. So a second LockFile must detect "already held" without ever
//      opening the file, or it would unlock the first holder on its way out.
//
// Files are identified by device and inode, not by path: "db/LOCK",
// "./db/LOCK", and a symlink to it are one file. The mutex is held across the
// whole LockFile call so the check and the insert cannot race.
struct LockRegistry {
  using FileId = std::pair<dev_t, ino_t>;
  std::mutex mutex;
  std::set<FileId> held;
};

class PosixFileLock final : public FileLock {
 public:
  PosixFileLock(LockRegistry* registry, LockRegistry::FileId id, int fd)
      : registry_(registry), id_(id), fd_(fd) {}

  // Closing the descriptor releases the fcntl lock.
  ~PosixFileLock() override {
    const std::lock_guard<std::mutex> guard(registry_->mutex);
    static_cast<void>(CloseDescriptor(fd_));
    registry_->held.erase(id_);
  }
  PosixFileLock(const PosixFileLock&) = delete;
  PosixFileLock& operator=(const PosixFileLock&) = delete;
  PosixFileLock(PosixFileLock&&) = delete;
  PosixFileLock& operator=(PosixFileLock&&) = delete;

 private:
  LockRegistry* registry_;
  LockRegistry::FileId id_;
  int fd_;
};

class PosixEnv final : public Env {
 public:
  Result<std::unique_ptr<SequentialFile>> NewSequentialFile(const std::string& path) override {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return ErrnoStatus(path, errno);
    }
    return std::unique_ptr<SequentialFile>(std::make_unique<PosixSequentialFile>(path, fd));
  }

  Result<std::unique_ptr<RandomAccessFile>> NewRandomAccessFile(const std::string& path) override {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return ErrnoStatus(path, errno);
    }
    return std::unique_ptr<RandomAccessFile>(std::make_unique<PosixRandomAccessFile>(path, fd));
  }

  Result<std::unique_ptr<WritableFile>> NewWritableFile(const std::string& path) override {
    return OpenForWriting(path, O_TRUNC);
  }

  Result<std::unique_ptr<WritableFile>> NewAppendableFile(const std::string& path) override {
    return OpenForWriting(path, O_APPEND);
  }

  bool FileExists(const std::string& path) override { return ::access(path.c_str(), F_OK) == 0; }

  Result<std::vector<std::string>> GetChildren(const std::string& dir) override {
    DIR* const handle = ::opendir(dir.c_str());
    if (handle == nullptr) {
      return ErrnoStatus(dir, errno);
    }
    std::vector<std::string> names;
    // readdir() returns null both at the end of the directory and on failure;
    // only errno tells them apart. Without this check a failed scan would look
    // like a shorter listing, and recovery would conclude files do not exist.
    int error_number = 0;
    while (true) {
      errno = 0;
      const dirent* entry = ::readdir(handle);
      if (entry == nullptr) {
        error_number = errno;
        break;
      }
      const std::string_view name = entry->d_name;
      if (name != "." && name != "..") {
        names.emplace_back(name);
      }
    }
    ::closedir(handle);
    if (error_number != 0) {
      return ErrnoStatus(dir, error_number);
    }
    return names;
  }

  Result<std::uint64_t> GetFileSize(const std::string& path) override {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
      return ErrnoStatus(path, errno);
    }
    return static_cast<std::uint64_t>(info.st_size);
  }

  Status RemoveFile(const std::string& path) override {
    if (::unlink(path.c_str()) != 0) {
      return ErrnoStatus(path, errno);
    }
    return Status::Ok();
  }

  Status RenameFile(const std::string& from, const std::string& to) override {
    if (::rename(from.c_str(), to.c_str()) != 0) {
      return ErrnoStatus(from + " -> " + to, errno);
    }
    return Status::Ok();
  }

  Status CreateDir(const std::string& path) override {
    if (::mkdir(path.c_str(), 0755) == 0) {
      return Status::Ok();
    }
    const int error_number = errno;
    struct stat info {};
    if (error_number == EEXIST && ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
      return Status::Ok();
    }
    return ErrnoStatus(path, error_number);
  }

  Status RemoveDir(const std::string& path) override {
    if (::rmdir(path.c_str()) != 0) {
      return ErrnoStatus(path, errno);
    }
    return Status::Ok();
  }

  Status SyncDir(const std::string& dir) override {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
      return ErrnoStatus(dir, errno);
    }
    Status status = Status::Ok();
    if (::fsync(fd) != 0) {
      status = ErrnoStatus(dir, errno);
    }
    static_cast<void>(CloseDescriptor(fd));
    return status;
  }

  Result<std::unique_ptr<FileLock>> LockFile(const std::string& path) override {
    const std::lock_guard<std::mutex> guard(locks_.mutex);

    // Check before opening: see LockRegistry for why the file must not be
    // opened (and so closed again) when this process already holds it.
    struct stat existing {};
    if (::stat(path.c_str(), &existing) == 0 &&
        locks_.held.contains(LockRegistry::FileId{existing.st_dev, existing.st_ino})) {
      return Status::Busy(path + ": already locked by this process");
    }

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      return ErrnoStatus(path, errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
      const int error_number = errno;
      static_cast<void>(CloseDescriptor(fd));
      return ErrnoStatus(path, error_number);
    }

    struct flock request {};
    request.l_type = F_WRLCK;
    request.l_whence = SEEK_SET;
    request.l_start = 0;
    request.l_len = 0;  // the whole file
    if (::fcntl(fd, F_SETLK, &request) != 0) {
      const int error_number = errno;
      static_cast<void>(CloseDescriptor(fd));
      if (error_number == EACCES || error_number == EAGAIN) {
        return Status::Busy(path + ": locked by another process");
      }
      return ErrnoStatus(path, error_number);
    }
    const LockRegistry::FileId id{info.st_dev, info.st_ino};
    locks_.held.insert(id);
    return std::unique_ptr<FileLock>(std::make_unique<PosixFileLock>(&locks_, id, fd));
  }

  std::uint64_t NowMicros() override {
    const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count());
  }

 private:
  static Result<std::unique_ptr<WritableFile>> OpenForWriting(const std::string& path,
                                                              int mode_flag) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | mode_flag, 0644);
    if (fd < 0) {
      return ErrnoStatus(path, errno);
    }
    return std::unique_ptr<WritableFile>(std::make_unique<PosixWritableFile>(path, fd));
  }

  LockRegistry locks_;
};

}  // namespace

Env* Env::Default() {
  // Leaked on purpose: background threads and static destructors may still use
  // the default Env while the process exits, and destroying it first would
  // turn that into a use-after-free.
  static Env* const kDefault = new PosixEnv();
  return kDefault;
}

}  // namespace strata

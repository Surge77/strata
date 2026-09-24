#pragma once

#include "strata/result.hpp"
#include "strata/slice.hpp"
#include "strata/status.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace strata {

// The engine's only way to touch the operating system.
//
// Every file the engine reads or writes, every directory it lists, and every
// fsync it issues goes through an Env. That single choke point is what makes
// the durability claims testable: a test can hand the engine an Env that
// models power loss, a full disk, or a failing fsync, and check that the engine
// reacts correctly -- without which "crash-safe" would be an untested belief.
//
// Env::Default() is the real POSIX implementation. Paths are plain strings in
// the platform's native form; the engine never interprets them beyond joining
// a database directory with a file name.

// Reads a file from front to back. Not safe for concurrent use.
class SequentialFile {
 public:
  SequentialFile() = default;
  virtual ~SequentialFile() = default;
  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;
  SequentialFile(SequentialFile&&) = delete;
  SequentialFile& operator=(SequentialFile&&) = delete;

  // Reads up to `n` bytes. `scratch` must have room for `n` bytes; the returned
  // Slice may point into it and is valid until the next call. A result shorter
  // than `n` -- including empty -- means end of file was reached.
  virtual Result<Slice> Read(std::size_t n, char* scratch) = 0;

  // Advances past `n` bytes without returning them. Skipping past the end of
  // the file is not an error; the next Read returns empty.
  virtual Status Skip(std::uint64_t n) = 0;
};

// Reads at arbitrary offsets. Safe for concurrent use by multiple threads,
// because SSTables are shared by every reader of the database.
class RandomAccessFile {
 public:
  RandomAccessFile() = default;
  virtual ~RandomAccessFile() = default;
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;
  RandomAccessFile(RandomAccessFile&&) = delete;
  RandomAccessFile& operator=(RandomAccessFile&&) = delete;

  // Reads up to `n` bytes starting at `offset`, with the same scratch and
  // short-read contract as SequentialFile::Read.
  virtual Result<Slice> Read(std::uint64_t offset, std::size_t n, char* scratch) const = 0;
};

// An append-only output file. Not safe for concurrent use.
//
// Three levels of "written", which the engine must never confuse:
//   Append  -- the bytes are in this object; a crash loses them.
//   Flush   -- the bytes are in the operating system; a process crash keeps
//              them, a power loss may not.
//   Sync    -- the bytes are on stable storage.
// Sync does not make the file's *name* durable. A newly created file can still
// vanish after power loss until Env::SyncDir is called on its directory.
//
// Errors are sticky: once Append, Flush, or Sync fails, every later call
// returns that failure. After a failed write it is unknown how much reached the
// file, and after a failed fsync the kernel may already have discarded the
// dirty pages, so a later successful Sync would not mean the data is durable.
// A caller that sees an error must abandon the file (a log writer starts a new
// log) rather than retry.
class WritableFile {
 public:
  WritableFile() = default;
  virtual ~WritableFile() = default;
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;
  WritableFile(WritableFile&&) = delete;
  WritableFile& operator=(WritableFile&&) = delete;

  virtual Status Append(Slice data) = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;

  // Flushes and releases the file. Calling any other method afterwards is a
  // programming error. Destroying an unclosed file closes it and discards the
  // error, so callers that care about the result must call Close explicitly.
  virtual Status Close() = 0;
};

// Ownership of a lock obtained from Env::LockFile. Destroying it releases the
// lock, so a database that fails halfway through opening cannot leak one.
class FileLock {
 public:
  FileLock() = default;
  virtual ~FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&&) = delete;
  FileLock& operator=(FileLock&&) = delete;
};

class Env {
 public:
  Env() = default;
  virtual ~Env() = default;
  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;
  Env(Env&&) = delete;
  Env& operator=(Env&&) = delete;

  // The process-wide POSIX environment. Never destroyed, so it is safe to use
  // from static destructors and background threads during shutdown.
  static Env* Default();

  // A missing file is NotFound; other failures are IoError.
  virtual Result<std::unique_ptr<SequentialFile>> NewSequentialFile(const std::string& path) = 0;
  virtual Result<std::unique_ptr<RandomAccessFile>> NewRandomAccessFile(
      const std::string& path) = 0;

  // Creates the file, truncating any existing contents.
  virtual Result<std::unique_ptr<WritableFile>> NewWritableFile(const std::string& path) = 0;

  // Creates the file if missing; otherwise appends to what is there.
  virtual Result<std::unique_ptr<WritableFile>> NewAppendableFile(const std::string& path) = 0;

  [[nodiscard]] virtual bool FileExists(const std::string& path) = 0;

  // Names (not paths) of the entries in `dir`, excluding "." and "..", in no
  // particular order.
  virtual Result<std::vector<std::string>> GetChildren(const std::string& dir) = 0;

  virtual Result<std::uint64_t> GetFileSize(const std::string& path) = 0;
  virtual Status RemoveFile(const std::string& path) = 0;

  // Atomically replaces `to` if it exists. Like every directory change, the
  // rename is not durable until SyncDir is called on the directory.
  virtual Status RenameFile(const std::string& from, const std::string& to) = 0;

  // Succeeds if the directory already exists.
  virtual Status CreateDir(const std::string& path) = 0;
  virtual Status RemoveDir(const std::string& path) = 0;

  // Makes every change to the entries of `dir` -- creations, renames, removals
  // -- durable. Syncing a file does not do this.
  virtual Status SyncDir(const std::string& dir) = 0;

  // Takes an exclusive lock on `path`, creating the file if needed. Returns
  // Busy if the lock is held, whether by another process or by this one: a
  // database must not be opened twice even from the same process.
  virtual Result<std::unique_ptr<FileLock>> LockFile(const std::string& path) = 0;

  // Microseconds from an arbitrary fixed point. Monotonic: use it to measure
  // durations, never as a wall-clock time.
  [[nodiscard]] virtual std::uint64_t NowMicros() = 0;
};

// An Env that forwards every call to another one.
//
// Test environments derive from this and override only the calls they change,
// e.g. a fault-injection Env that intercepts Sync and SyncDir but reads and
// writes real files.
class EnvWrapper : public Env {
 public:
  // `target` must outlive the wrapper.
  explicit EnvWrapper(Env* target) : target_(target) {}

  [[nodiscard]] Env* target() const noexcept { return target_; }

  Result<std::unique_ptr<SequentialFile>> NewSequentialFile(const std::string& path) override {
    return target_->NewSequentialFile(path);
  }
  Result<std::unique_ptr<RandomAccessFile>> NewRandomAccessFile(const std::string& path) override {
    return target_->NewRandomAccessFile(path);
  }
  Result<std::unique_ptr<WritableFile>> NewWritableFile(const std::string& path) override {
    return target_->NewWritableFile(path);
  }
  Result<std::unique_ptr<WritableFile>> NewAppendableFile(const std::string& path) override {
    return target_->NewAppendableFile(path);
  }
  bool FileExists(const std::string& path) override { return target_->FileExists(path); }
  Result<std::vector<std::string>> GetChildren(const std::string& dir) override {
    return target_->GetChildren(dir);
  }
  Result<std::uint64_t> GetFileSize(const std::string& path) override {
    return target_->GetFileSize(path);
  }
  Status RemoveFile(const std::string& path) override { return target_->RemoveFile(path); }
  Status RenameFile(const std::string& from, const std::string& to) override {
    return target_->RenameFile(from, to);
  }
  Status CreateDir(const std::string& path) override { return target_->CreateDir(path); }
  Status RemoveDir(const std::string& path) override { return target_->RemoveDir(path); }
  Status SyncDir(const std::string& dir) override { return target_->SyncDir(dir); }
  Result<std::unique_ptr<FileLock>> LockFile(const std::string& path) override {
    return target_->LockFile(path);
  }
  std::uint64_t NowMicros() override { return target_->NowMicros(); }

 private:
  Env* target_;
};

}  // namespace strata

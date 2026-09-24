#pragma once

#include "strata/env.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace strata::test {

// A WritableFile that appends to a caller-owned string, so format tests can
// inspect exact bytes without touching a disk. Can be told to start failing.
class StringSink final : public WritableFile {
 public:
  explicit StringSink(std::string* contents) : contents_(contents) {}

  Status Append(Slice data) override {
    if (fail_after_ != kNever && contents_->size() + data.size() > fail_after_) {
      return Status::IoError("injected append failure");
    }
    contents_->append(data);
    return Status::Ok();
  }
  Status Flush() override {
    return fail_flush_ ? Status::IoError("injected flush failure") : Status::Ok();
  }
  Status Sync() override { return Status::Ok(); }
  Status Close() override { return Status::Ok(); }

  // Appends that would grow the contents past `bytes` fail.
  void FailAfter(std::size_t bytes) { fail_after_ = bytes; }

  void FailFlush() { fail_flush_ = true; }

 private:
  static constexpr std::size_t kNever = static_cast<std::size_t>(-1);
  std::string* contents_;
  std::size_t fail_after_ = kNever;
  bool fail_flush_ = false;
};

// A SequentialFile over an in-memory string. Can be told to fail a read.
class StringSource final : public SequentialFile {
 public:
  explicit StringSource(std::string contents) : contents_(std::move(contents)) {}

  Result<Slice> Read(std::size_t n, char* scratch) override {
    if (fail_reads_) {
      return Status::IoError("injected read failure");
    }
    const std::size_t count = std::min(n, contents_.size() - position_);
    std::memcpy(scratch, contents_.data() + position_, count);
    position_ += count;
    return Slice(scratch, count);
  }

  Status Skip(std::uint64_t n) override {
    position_ = std::min<std::uint64_t>(contents_.size(), position_ + n);
    return Status::Ok();
  }

  void FailReads() { fail_reads_ = true; }

 private:
  std::string contents_;
  std::size_t position_ = 0;
  bool fail_reads_ = false;
};

// A RandomAccessFile over an in-memory string (fuzzing and table tests).
class StringRandomAccessFile final : public RandomAccessFile {
 public:
  explicit StringRandomAccessFile(std::string contents) : contents_(std::move(contents)) {}

  Result<Slice> Read(std::uint64_t offset, std::size_t n, char* scratch) const override {
    if (offset >= contents_.size()) {
      return Slice(scratch, 0);
    }
    const std::size_t count = std::min<std::size_t>(n, contents_.size() - offset);
    std::memcpy(scratch, contents_.data() + offset, count);
    return Slice(scratch, count);
  }

 private:
  std::string contents_;
};

}  // namespace strata::test

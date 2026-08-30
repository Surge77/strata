#pragma once

#include "strata/slice.hpp"

#include <memory>
#include <string>

namespace strata {

// The outcome of an operation: a code, plus an optional human-readable message.
//
// The engine never throws across an API boundary, so every fallible operation
// returns a Status (or a Result<T>, which carries one on failure). Ignoring one
// is a bug, hence [[nodiscard]] on the class itself.
//
// Layout note: the message is heap-allocated only when there is one, so a
// successful call constructs, copies, and destroys a Status without touching
// the allocator -- which matters because the success path is the hot path.
class [[nodiscard]] Status {
 public:
  enum class Code : std::uint8_t {
    kOk = 0,
    kNotFound,         // the key is absent, or was deleted
    kCorruption,       // an on-disk structure failed its checksum or invariants
    kInvalidArgument,  // the caller passed something the API forbids
    kIoError,          // the operating system refused or failed an I/O request
    kNotSupported,     // a valid request this build cannot serve
    kAlreadyExists,    // creating something that is already there
    kBusy,             // a resource is held elsewhere, e.g. the database lock
    kInternal,         // an invariant this code is responsible for was violated
  };

  Status() noexcept = default;

  Status(const Status& other);
  Status& operator=(const Status& other);
  Status(Status&&) noexcept = default;
  Status& operator=(Status&&) noexcept = default;
  ~Status() = default;

  static Status Ok() noexcept { return {}; }

  static Status NotFound(Slice message = {}) { return Status(Code::kNotFound, message); }
  static Status Corruption(Slice message = {}) { return Status(Code::kCorruption, message); }
  static Status InvalidArgument(Slice m = {}) { return Status(Code::kInvalidArgument, m); }
  static Status IoError(Slice message = {}) { return Status(Code::kIoError, message); }
  static Status NotSupported(Slice message = {}) { return Status(Code::kNotSupported, message); }
  static Status AlreadyExists(Slice message = {}) { return Status(Code::kAlreadyExists, message); }
  static Status Busy(Slice message = {}) { return Status(Code::kBusy, message); }
  static Status Internal(Slice message = {}) { return Status(Code::kInternal, message); }

  [[nodiscard]] Code code() const noexcept { return code_; }

  [[nodiscard]] bool ok() const noexcept { return code_ == Code::kOk; }
  [[nodiscard]] bool IsNotFound() const noexcept { return code_ == Code::kNotFound; }
  [[nodiscard]] bool IsCorruption() const noexcept { return code_ == Code::kCorruption; }
  [[nodiscard]] bool IsIoError() const noexcept { return code_ == Code::kIoError; }
  [[nodiscard]] bool IsBusy() const noexcept { return code_ == Code::kBusy; }

  // Empty when no message was supplied. Never contains the code name.
  [[nodiscard]] Slice message() const noexcept { return message_ ? Slice(*message_) : Slice{}; }

  // "Ok", or "Corruption: bad block checksum". Suitable for logs and CLI output.
  [[nodiscard]] std::string ToString() const;

  // Name of the code alone, e.g. "Corruption".
  [[nodiscard]] static Slice CodeName(Code code) noexcept;

  friend bool operator==(const Status& lhs, const Status& rhs) noexcept {
    return lhs.code_ == rhs.code_;
  }

 private:
  Status(Code code, Slice message);

  Code code_ = Code::kOk;
  std::unique_ptr<std::string> message_;
};

}  // namespace strata

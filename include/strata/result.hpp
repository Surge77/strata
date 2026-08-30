#pragma once

#include "strata/status.hpp"

#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>

namespace strata {

// Either a value or the Status explaining why there isn't one.
//
// Operations that can fail *and* produce something return Result<T>; operations
// that can only fail return Status. This is the whole error model -- the engine
// does not throw across an API boundary (see docs/decisions/ADR-0003).
//
// This is not std::expected only because the project's baseline is C++20 and
// <expected> is C++23. The interface is deliberately a subset of it, so
// migrating later is a mechanical change.
template <typename T>
class [[nodiscard]] Result {
  static_assert(!std::is_same_v<std::remove_cvref_t<T>, Status>,
                "Result<Status> is ambiguous; return Status on its own");
  static_assert(!std::is_reference_v<T>, "Result cannot hold a reference");

 public:
  using value_type = T;

  Result(T value) : storage_(std::move(value)) {}  // NOLINT(google-explicit-constructor)

  // Implicit so that `return Status::Corruption(...)` works in a function
  // returning Result<T>, which is the common failure path.
  Result(Status status)  // NOLINT(google-explicit-constructor)
      : storage_(std::move(status)) {
    // An "ok failure" would make ok() and status().ok() disagree; every caller
    // reading the value would then read a default-constructed T.
    assert(!std::get<Status>(storage_).ok() && "Result constructed from an ok Status");
  }

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
  explicit operator bool() const noexcept { return ok(); }

  // Precondition: ok(). Checked with assert in debug builds.
  [[nodiscard]] const T& value() const& {
    assert(ok() && "Result::value() on a failed Result");
    return std::get<T>(storage_);
  }
  [[nodiscard]] T& value() & {
    assert(ok() && "Result::value() on a failed Result");
    return std::get<T>(storage_);
  }
  [[nodiscard]] T&& value() && {
    assert(ok() && "Result::value() on a failed Result");
    return std::get<T>(std::move(storage_));
  }

  const T& operator*() const& { return value(); }
  T& operator*() & { return value(); }
  T&& operator*() && { return std::move(*this).value(); }
  const T* operator->() const { return &value(); }
  T* operator->() { return &value(); }

  // Ok() when a value is held, otherwise the failure.
  [[nodiscard]] Status status() const {
    const Status* failure = std::get_if<Status>(&storage_);
    return failure != nullptr ? *failure : Status::Ok();
  }

  template <typename U>
  [[nodiscard]] T value_or(U&& fallback) const& {
    return ok() ? value() : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  std::variant<T, Status> storage_;
};

}  // namespace strata

// --- Error-propagation macros -------------------------------------------------
//
// Defined after the namespace because macros are not scoped, and CONCAT first
// because a macro must exist before the macro that expands it is invoked.

#define STRATA_CONCAT_INNER(a, b) a##b
#define STRATA_CONCAT(a, b) STRATA_CONCAT_INNER(a, b)

// Propagates a failure out of the enclosing function, otherwise binds the value:
//
//   STRATA_ASSIGN_OR_RETURN(auto file, env.OpenSequential(path));
//
// The enclosing function must return Status or Result<U>. The expression is
// evaluated exactly once, and the temporary is named after __LINE__ so two uses
// in one scope do not collide.
#define STRATA_ASSIGN_OR_RETURN(declaration, expression)       \
  auto STRATA_CONCAT(strata_result_, __LINE__) = (expression); \
  if (!STRATA_CONCAT(strata_result_, __LINE__).ok()) {         \
    return STRATA_CONCAT(strata_result_, __LINE__).status();   \
  }                                                            \
  declaration = std::move(STRATA_CONCAT(strata_result_, __LINE__)).value()

// Returns early if `expression` yields a failed Status.
#define STRATA_RETURN_IF_ERROR(expression)                                   \
  do {                                                                       \
    ::strata::Status STRATA_CONCAT(strata_status_, __LINE__) = (expression); \
    if (!STRATA_CONCAT(strata_status_, __LINE__).ok()) {                     \
      return STRATA_CONCAT(strata_status_, __LINE__);                        \
    }                                                                        \
  } while (false)

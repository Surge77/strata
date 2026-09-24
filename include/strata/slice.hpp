#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace strata {

// A non-owning view of a contiguous byte range.
//
// This is deliberately an alias for std::string_view rather than a bespoke
// class (see docs/decisions/ADR-0002). Keys and values are arbitrary bytes,
// including embedded NULs, which string_view handles: its length is explicit
// and never derived from a terminator. Its ordering is also the ordering this
// engine needs -- std::char_traits<char>::compare is specified to behave like
// memcmp, so comparison is unsigned lexicographic regardless of whether the
// platform's `char` is signed.
using Slice = std::string_view;

// Reinterprets a byte span as a Slice.
//
// Byte-oriented call sites (block buffers, file reads) hold std::byte; the
// engine's key/value vocabulary is Slice. These two helpers are the only place
// that cast happens, which keeps reinterpret_cast out of the rest of the code.
[[nodiscard]] inline Slice AsSlice(std::span<const std::byte> bytes) noexcept {
  // Safe: any object may be examined through char, and the ranges are identical.
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] inline std::span<const std::byte> AsBytes(Slice slice) noexcept {
  return {reinterpret_cast<const std::byte*>(slice.data()), slice.size()};
}

// Copies a Slice into an owning string. Named so that allocation at a call site
// is obvious rather than implied by an implicit conversion.
[[nodiscard]] inline std::string ToString(Slice slice) {
  return std::string(slice);
}

// Length of the longest common prefix of `a` and `b`.
//
// SSTable data blocks store each key as a shared-prefix length plus the
// remaining bytes, so this runs once per key written.
[[nodiscard]] inline std::size_t SharedPrefixLength(Slice a, Slice b) noexcept {
  const std::size_t limit = a.size() < b.size() ? a.size() : b.size();
  std::size_t shared = 0;
  while (shared < limit && a[shared] == b[shared]) {
    ++shared;
  }
  return shared;
}

}  // namespace strata

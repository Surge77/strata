#pragma once

#include "strata/slice.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

namespace strata {

// Encoding primitives for every on-disk structure in the engine.
//
// Two rules this file exists to enforce:
//
//   1. Fixed-width integers are little-endian on disk regardless of host byte
//      order, so a database file is portable between machines.
//   2. Every decode is bounds-checked against the buffer it was handed and
//      reports failure instead of reading past the end. On-disk bytes are
//      untrusted input: a corrupted length field must produce a Corruption
//      status, not a segfault or a multi-gigabyte allocation.

namespace internal {

// std::byteswap is C++23 and this project's baseline is C++20. Only reached on
// a big-endian host, where these branches are the difference between a portable
// database file and a corrupt one.
[[nodiscard]] constexpr std::uint32_t ByteSwap(std::uint32_t value) noexcept {
  return ((value & 0x000000FFU) << 24U) | ((value & 0x0000FF00U) << 8U) |
         ((value & 0x00FF0000U) >> 8U) | ((value & 0xFF000000U) >> 24U);
}

[[nodiscard]] constexpr std::uint64_t ByteSwap(std::uint64_t value) noexcept {
  return ((value & 0x00000000000000FFULL) << 56U) | ((value & 0x000000000000FF00ULL) << 40U) |
         ((value & 0x0000000000FF0000ULL) << 24U) | ((value & 0x00000000FF000000ULL) << 8U) |
         ((value & 0x000000FF00000000ULL) >> 8U) | ((value & 0x0000FF0000000000ULL) >> 24U) |
         ((value & 0x00FF000000000000ULL) >> 40U) | ((value & 0xFF00000000000000ULL) >> 56U);
}

}  // namespace internal

// Maximum bytes a varint of each width can occupy.
inline constexpr int kMaxVarint32Length = 5;
inline constexpr int kMaxVarint64Length = 10;

// --- fixed width -------------------------------------------------------------

inline void EncodeFixed32(char* destination, std::uint32_t value) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    value = internal::ByteSwap(value);
  }
  std::memcpy(destination, &value, sizeof(value));
}

inline void EncodeFixed64(char* destination, std::uint64_t value) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    value = internal::ByteSwap(value);
  }
  std::memcpy(destination, &value, sizeof(value));
}

// Precondition: at least 4 (resp. 8) readable bytes at the source pointer.
// Callers reading from a file buffer must check the remaining length first.
[[nodiscard]] inline std::uint32_t DecodeFixed32(const char* source) noexcept {
  std::uint32_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  if constexpr (std::endian::native == std::endian::big) {
    value = internal::ByteSwap(value);
  }
  return value;
}

[[nodiscard]] inline std::uint64_t DecodeFixed64(const char* source) noexcept {
  std::uint64_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  if constexpr (std::endian::native == std::endian::big) {
    value = internal::ByteSwap(value);
  }
  return value;
}

void PutFixed32(std::string* destination, std::uint32_t value);
void PutFixed64(std::string* destination, std::uint64_t value);

// --- varint ------------------------------------------------------------------
//
// Base-128 with a continuation bit in the high position of each byte, least
// significant group first. Small values, which lengths and counts almost always
// are, cost one byte instead of four or eight.

[[nodiscard]] int VarintLength(std::uint64_t value) noexcept;

// Writes at most kMaxVarint64Length bytes; returns one past the last written.
char* EncodeVarint32(char* destination, std::uint32_t value) noexcept;
char* EncodeVarint64(char* destination, std::uint64_t value) noexcept;

void PutVarint32(std::string* destination, std::uint32_t value);
void PutVarint64(std::string* destination, std::uint64_t value);

// On success, advances the input past the varint and returns true. On a
// truncated or over-long encoding, returns false and leaves the input untouched.
[[nodiscard]] bool GetVarint32(Slice* input, std::uint32_t* value) noexcept;
[[nodiscard]] bool GetVarint64(Slice* input, std::uint64_t* value) noexcept;

// A varint length followed by that many bytes. Decoding returns false if the
// length field is malformed or claims more bytes than the input holds. That
// check is what stops a corrupt file from being trusted about its own size.
void PutLengthPrefixedSlice(std::string* destination, Slice value);
[[nodiscard]] bool GetLengthPrefixedSlice(Slice* input, Slice* value) noexcept;

}  // namespace strata

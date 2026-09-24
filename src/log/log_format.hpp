#pragma once

#include <cstddef>
#include <cstdint>

// Constants shared by the log writer and reader. The byte layout they describe
// is specified in docs/format/log.md; change the two together or not at all.

namespace strata::log {

enum class RecordType : std::uint8_t {
  kZero = 0,  // never written: marks zero-filled space
  kFull = 1,
  kFirst = 2,
  kMiddle = 3,
  kLast = 4,
};

inline constexpr std::uint8_t kMaxRecordType = static_cast<std::uint8_t>(RecordType::kLast);

inline constexpr std::size_t kBlockSize = 32768;

// checksum (4) + length (2) + type (1)
inline constexpr std::size_t kHeaderSize = 4 + 2 + 1;

}  // namespace strata::log

#pragma once

#include <cstdint>

namespace strata {

// The kind of record stored for a key. The values are part of the on-disk
// format (WAL records, and from phase 3 the trailer of every internal key), so
// they are fixed forever.
enum class ValueType : std::uint8_t {
  kDeletion = 0x0,  // a tombstone: the key is gone as of this sequence number
  kValue = 0x1,
};

// Sequence numbers are 56 bits: an internal key packs sequence and type into
// one 64-bit trailer.
using SequenceNumber = std::uint64_t;
inline constexpr SequenceNumber kMaxSequenceNumber = (SequenceNumber{1} << 56U) - 1U;

}  // namespace strata

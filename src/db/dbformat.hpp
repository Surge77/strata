#pragma once

#include "strata/slice.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace strata {

// The internal key format: what the engine actually sorts and stores, as
// opposed to the user's keys. Specified in docs/format/internal_key.md.

// The kind of record stored for a key. The values are part of the on-disk
// format (WAL records and the trailer of every internal key), so they are fixed
// forever.
enum class ValueType : std::uint8_t {
  kDeletion = 0x0,  // a tombstone: the key is gone as of this sequence number
  kValue = 0x1,
};

// Sequence numbers are 56 bits: an internal key packs sequence and type into
// one 64-bit trailer.
using SequenceNumber = std::uint64_t;
inline constexpr SequenceNumber kMaxSequenceNumber = (SequenceNumber{1} << 56U) - 1U;

// When seeking to "the newest version of user_key at or below sequence s", the
// seek key must sort before every real entry for (user_key, s). Entries with
// equal sequence sort by type descending, so the seek key uses the highest
// type.
inline constexpr ValueType kValueTypeForSeek = ValueType::kValue;

// Bytes in the trailer that follows the user key.
inline constexpr std::size_t kInternalKeyTrailerSize = 8;

[[nodiscard]] constexpr std::uint64_t PackSequenceAndType(SequenceNumber sequence,
                                                          ValueType type) noexcept {
  return (sequence << 8U) | static_cast<std::uint8_t>(type);
}

struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence = 0;
  ValueType type = ValueType::kValue;
};

// Appends user_key || fixed64(sequence << 8 | type).
void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// Returns false if `internal_key` is shorter than a trailer or carries an
// unknown type -- on-disk keys are untrusted.
[[nodiscard]] bool ParseInternalKey(Slice internal_key, ParsedInternalKey* result) noexcept;

// Precondition: internal_key.size() >= kInternalKeyTrailerSize.
[[nodiscard]] inline Slice ExtractUserKey(Slice internal_key) noexcept {
  return internal_key.substr(0, internal_key.size() - kInternalKeyTrailerSize);
}

// Orders internal keys by user key ascending (bytewise), then by sequence
// number descending, then by type descending. "Descending sequence" is what
// makes the newest version of a key the first one a seek finds, while older
// versions stay reachable for snapshots.
class InternalKeyComparator {
 public:
  [[nodiscard]] int Compare(Slice a, Slice b) const noexcept;
};

// The key a point lookup searches for: every representation the memtable and
// SSTables need, built once per Get.
//
//   memtable_key = varint32(internal key length) || internal key
//   internal_key = user_key || fixed64(sequence << 8 | kValueTypeForSeek)
class LookupKey {
 public:
  LookupKey(Slice user_key, SequenceNumber sequence);

  [[nodiscard]] Slice memtable_key() const noexcept { return storage_; }
  [[nodiscard]] Slice internal_key() const noexcept {
    return Slice(storage_).substr(internal_key_offset_);
  }
  [[nodiscard]] Slice user_key() const noexcept {
    return internal_key().substr(0, internal_key().size() - kInternalKeyTrailerSize);
  }

 private:
  std::string storage_;
  std::size_t internal_key_offset_ = 0;
};

}  // namespace strata

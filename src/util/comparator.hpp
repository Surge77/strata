#pragma once

#include "strata/slice.hpp"

#include <string>
#include <string_view>

namespace strata {

// A total order over keys, plus the two operations that let an SSTable index
// store short keys instead of full ones.
//
// Implementations must be thread-safe (they are shared by every reader) and
// stateless in the sense that matters: the same inputs always order the same
// way, forever, because the order is baked into files on disk.
class Comparator {
 public:
  Comparator() = default;
  virtual ~Comparator() = default;
  Comparator(const Comparator&) = delete;
  Comparator& operator=(const Comparator&) = delete;
  Comparator(Comparator&&) = delete;
  Comparator& operator=(Comparator&&) = delete;

  // <0, 0, or >0 as a sorts before, equal to, or after b.
  [[nodiscard]] virtual int Compare(Slice a, Slice b) const = 0;

  // Identifies the ordering; recorded in files so a database is never opened
  // with a different order than it was written with (phase 5).
  [[nodiscard]] virtual std::string_view Name() const = 0;

  // If *start < limit, may shorten *start to any key in [*start, limit). Used
  // for index-block separators: any key between two blocks works, so the
  // shortest one is stored.
  virtual void FindShortestSeparator(std::string* start, Slice limit) const = 0;

  // May change *key to any short key >= *key. Used for the last index entry.
  virtual void FindShortSuccessor(std::string* key) const = 0;
};

// Unsigned lexicographic byte order -- memcmp order. The order of user keys.
// Never destroyed.
[[nodiscard]] const Comparator* BytewiseComparator();

}  // namespace strata

#pragma once

#include "strata/slice.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "util/comparator.hpp"

namespace strata::table {

// Builds one block in the format of docs/format/sstable.md: prefix-compressed
// entries, then the restart array.
//
// Keys must be added in strictly increasing order under `comparator`; the
// builder asserts it in debug builds (the table builder, which receives keys
// from callers, checks it for real).
class BlockBuilder {
 public:
  // Prefix compression restarts every `restart_interval` entries: a longer
  // interval saves space, a shorter one makes seeks scan fewer entries.
  BlockBuilder(const Comparator* comparator, int restart_interval);

  // Starts a new, empty block.
  void Reset();

  void Add(Slice key, Slice value);

  // Appends the restart array and returns the block contents, valid until the
  // next Reset. No Add is allowed after Finish.
  Slice Finish();

  // Bytes the block would occupy if finished now.
  [[nodiscard]] std::size_t CurrentSizeEstimate() const noexcept;

  [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }

 private:
  const Comparator* comparator_;
  int restart_interval_;
  std::string buffer_;
  std::vector<std::uint32_t> restarts_;
  int entries_since_restart_ = 0;
  bool finished_ = false;
  std::string last_key_;
};

}  // namespace strata::table

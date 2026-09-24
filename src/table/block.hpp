#pragma once

#include "strata/iterator.hpp"
#include "strata/result.hpp"

#include <cstdint>
#include <memory>
#include <string>

#include "util/comparator.hpp"

namespace strata::table {

// A parsed, immutable block (docs/format/sstable.md, "Block contents").
//
// Create validates the restart array up front, so iterators can trust every
// restart offset. Entries are still decoded lazily and bounds-checked one by
// one: a malformed entry makes the iterator invalid with a Corruption status
// rather than reading out of bounds. Blocks that reach here have already
// passed their checksum, so malformed entries mean a bug or a checksum
// collision -- rare, but never allowed to become memory unsafety.
//
// Safe to share between threads; each iterator is independent.
class Block {
 public:
  static Result<std::unique_ptr<Block>> Create(std::string contents);

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;
  Block(Block&&) = delete;
  Block& operator=(Block&&) = delete;
  ~Block() = default;

  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

  // The iterator must not outlive the Block.
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator(const Comparator* comparator) const;

 private:
  Block(std::string contents, std::uint32_t restart_offset, std::uint32_t num_restarts)
      : data_(std::move(contents)), restart_offset_(restart_offset), num_restarts_(num_restarts) {}

  std::string data_;
  std::uint32_t restart_offset_;  // where the restart array begins
  std::uint32_t num_restarts_;
};

}  // namespace strata::table

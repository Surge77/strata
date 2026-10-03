#pragma once

#include "strata/env.hpp"
#include "strata/iterator.hpp"
#include "strata/result.hpp"
#include "strata/slice.hpp"
#include "strata/status.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "table/block.hpp"
#include "table/table_builder.hpp"

namespace strata::table {

// An open, immutable SSTable.
//
// Open reads and verifies the footer, the index block, and the metaindex, so
// a table that opens has a trustworthy map of its data blocks. Data blocks are
// read (and checksum-verified) on demand. Safe to use from many threads at
// once; each iterator is independent.
class Table {
 public:
  static Result<std::unique_ptr<Table>> Open(const TableOptions& options,
                                             std::unique_ptr<RandomAccessFile> file,
                                             std::uint64_t file_size);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;
  Table(Table&&) = delete;
  Table& operator=(Table&&) = delete;
  ~Table() = default;

  // Iterates every entry in key order. Iteration stops at the first block
  // that cannot be read or verified; status() then says why. It never skips
  // a damaged block and carries on, because a caller copying the table (as
  // compaction does) would silently drop that block's data.
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator() const;

  // Finds the first entry with key >= `target` and calls `found` with it, if
  // there is one. The point-lookup path: one index search, one block read.
  Status Seek(Slice target, const std::function<void(Slice key, Slice value)>& found) const;

  // A hint for exact user-key lookups. False means this table cannot contain
  // the key; true requires a normal Seek. Older tables always return true.
  [[nodiscard]] bool MayContainUserKey(Slice user_key) const noexcept;

  // Approximate file offset where `key` would be, for size estimates.
  [[nodiscard]] std::uint64_t ApproximateOffsetOf(Slice key) const;

 private:
  Table(const TableOptions& options, std::unique_ptr<RandomAccessFile> file,
        std::uint64_t file_size, std::unique_ptr<Block> index, std::uint64_t metaindex_offset,
        std::optional<std::string> filter)
      : options_(options),
        file_(std::move(file)),
        file_size_(file_size),
        index_(std::move(index)),
        metaindex_offset_(metaindex_offset),
        filter_(std::move(filter)) {}

  // Reads the data block an index entry points to.
  Result<std::unique_ptr<Iterator>> BlockIterator(Slice index_value) const;

  const TableOptions options_;
  const std::unique_ptr<RandomAccessFile> file_;
  const std::uint64_t file_size_;
  const std::unique_ptr<Block> index_;
  const std::uint64_t metaindex_offset_;
  const std::optional<std::string> filter_;
};

}  // namespace strata::table

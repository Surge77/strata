#pragma once

#include "strata/env.hpp"
#include "strata/slice.hpp"
#include "strata/status.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "table/block_builder.hpp"
#include "table/block_cache.hpp"
#include "table/format.hpp"
#include "util/comparator.hpp"

namespace strata::table {

struct TableOptions {
  // Orders the keys; for the engine's tables, the internal key comparator.
  const Comparator* comparator = nullptr;
  // A data block is cut after the entry that brings it past this size.
  std::size_t block_size = 4096;
  int block_restart_interval = 16;
  // Store a whole-table Bloom filter over the user part of internal keys.
  bool use_filter = false;
  // Reader only. Data blocks are looked up here before the file is read.
  // Null means no caching. Must outlive the table.
  BlockCache* block_cache = nullptr;
  // Reader only. Distinguishes this table's blocks in a shared block cache;
  // every table sharing one cache needs a different id.
  std::uint64_t cache_id = 0;
};

// Writes an SSTable (docs/format/sstable.md) to a file, one key at a time, in
// strictly increasing key order.
//
// The builder never syncs: making the finished file durable -- and its name,
// via SyncDir -- is the caller's decision, because only the caller knows when
// the table becomes part of the database (phase 5).
class TableBuilder {
 public:
  // `file` must outlive the builder. `options.comparator` must be set.
  TableBuilder(const TableOptions& options, WritableFile* file);

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;
  TableBuilder(TableBuilder&&) = delete;
  TableBuilder& operator=(TableBuilder&&) = delete;
  ~TableBuilder() = default;

  // InvalidArgument if `key` is not greater than every key added before. The
  // first error sticks: every later call returns it.
  Status Add(Slice key, Slice value);

  // Writes the remaining data block, the metaindex, the index, and the footer.
  Status Finish();

  // Stops without finishing; the file is left incomplete and must be deleted.
  void Abandon() { closed_ = true; }

  [[nodiscard]] std::uint64_t NumEntries() const noexcept { return num_entries_; }

  // Bytes written so far; after Finish, the size of the table.
  [[nodiscard]] std::uint64_t FileSize() const noexcept { return offset_; }

  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  // Writes the current data block, if any, and remembers its handle so its
  // index entry can be added once the next key is known.
  void FlushDataBlock();
  Status WriteBlock(BlockBuilder* block, BlockHandle* handle);
  Status WriteRawBlock(Slice contents, BlockHandle* handle);

  const TableOptions options_;
  WritableFile* const file_;
  std::uint64_t offset_ = 0;
  Status status_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::string last_key_;
  std::uint64_t num_entries_ = 0;
  std::vector<std::uint64_t> filter_hashes_;
  bool closed_ = false;

  // The index entry for a data block is added only when the *next* key
  // arrives: knowing both the block's last key and the next block's first key
  // lets the index store the shortest separator between them rather than a
  // full key. Until then the finished block's handle waits here.
  bool pending_index_entry_ = false;
  BlockHandle pending_handle_;
};

}  // namespace strata::table

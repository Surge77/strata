#pragma once

#include "strata/env.hpp"
#include "strata/iterator.hpp"
#include "strata/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "db/dbformat.hpp"
#include "table/block_cache.hpp"
#include "table/table.hpp"
#include "util/lru_cache.hpp"

namespace strata {

// Keeps recently used SSTables open: opening one reads its footer, index, and
// filter, which is too expensive to repeat per lookup. At most `capacity`
// tables stay open; the least recently used is closed first.
//
// Closing a table never disturbs a reader: tables are shared_ptrs, so a
// lookup holding one keeps it open until it finishes. A closed table is
// reopened on its next use, and its file is still there, because obsolete-file
// cleanup only deletes files no live Version references.
class TableCache {
 public:
  // `block_cache` may be null. The cache shares its ownership, so iterators
  // holding the TableCache keep it alive too.
  TableCache(std::string dbname, Env* env, const InternalKeyComparator* comparator,
             std::size_t capacity, std::shared_ptr<table::BlockCache> block_cache)
      : dbname_(std::move(dbname)),
        env_(env),
        comparator_(comparator),
        block_cache_(std::move(block_cache)),
        tables_(capacity) {}

  Result<std::shared_ptr<const table::Table>> Find(std::uint64_t number, std::uint64_t file_size);

  // An iterator over the table that keeps it open for as long as it lives,
  // even if the cache evicts it meanwhile.
  Result<std::unique_ptr<Iterator>> NewIterator(std::uint64_t number, std::uint64_t file_size);
  void Evict(std::uint64_t number);

 private:
  // Each open table counts as one unit of capacity.
  static constexpr std::size_t kTableCharge = 1;

  const std::string dbname_;
  Env* const env_;
  const InternalKeyComparator* const comparator_;
  const std::shared_ptr<table::BlockCache> block_cache_;
  LruCache<std::uint64_t, table::Table> tables_;
};

}  // namespace strata

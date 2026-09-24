#pragma once

#include "strata/env.hpp"
#include "strata/result.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "db/dbformat.hpp"
#include "table/table.hpp"

namespace strata {

// Keeps SSTables open once opened: opening one reads its footer and index,
// which is too expensive to repeat per lookup. Phase 6 bounds this with an
// LRU; for now every table stays open until evicted (on deletion). Tables are
// shared_ptrs so a reader keeps one alive even if it is evicted mid-read.
class TableCache {
 public:
  TableCache(std::string dbname, Env* env, const InternalKeyComparator* comparator)
      : dbname_(std::move(dbname)), env_(env), comparator_(comparator) {}

  Result<std::shared_ptr<const table::Table>> Find(std::uint64_t number, std::uint64_t file_size);
  void Evict(std::uint64_t number);

 private:
  const std::string dbname_;
  Env* const env_;
  const InternalKeyComparator* const comparator_;
  std::mutex mutex_;
  std::unordered_map<std::uint64_t, std::shared_ptr<const table::Table>> tables_;
};

}  // namespace strata

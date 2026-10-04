#include "db/table_cache.hpp"

#include "db/filename.hpp"

namespace strata {

Result<std::shared_ptr<const table::Table>> TableCache::Find(std::uint64_t number,
                                                             std::uint64_t file_size) {
  if (auto cached = tables_.Lookup(number)) {
    return cached;
  }
  // Opened outside any lock: it does I/O. Two threads may race to open the
  // same table; Insert keeps the first copy and the loser's is dropped.
  const std::string file_name = TableFileName(dbname_, number);
  auto file = env_->NewRandomAccessFile(file_name);
  if (file.status().IsNotFound()) {
    // A table the MANIFEST references is gone. Reporting NotFound would let
    // Get answer "no such key" and hide the data loss.
    return Status::Corruption(file_name + ": table referenced by the MANIFEST is missing");
  }
  STRATA_RETURN_IF_ERROR(file.status());
  // File numbers are never reused within a process, so they are safe block
  // cache ids.
  STRATA_ASSIGN_OR_RETURN(
      auto opened, table::Table::Open(
                       {.comparator = comparator_, .block_cache = block_cache_, .cache_id = number},
                       std::move(*file), file_size));
  std::shared_ptr<const table::Table> table = std::move(opened);
  return tables_.Insert(number, std::move(table), kTableCharge);
}

// The deleted table's cached blocks are left to age out of the block cache:
// its id is never reused, so they can never be served again.
void TableCache::Evict(std::uint64_t number) {
  tables_.Erase(number);
}

}  // namespace strata

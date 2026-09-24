#include "db/table_cache.hpp"

#include "db/filename.hpp"

namespace strata {

Result<std::shared_ptr<const table::Table>> TableCache::Find(std::uint64_t number,
                                                             std::uint64_t file_size) {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (const auto found = tables_.find(number); found != tables_.end()) {
      return found->second;
    }
  }
  // Opened outside the lock: it does I/O. Two threads may race to open the
  // same table; the loser's copy is simply dropped.
  const std::string file_name = TableFileName(dbname_, number);
  auto file = env_->NewRandomAccessFile(file_name);
  if (file.status().IsNotFound()) {
    // A table the MANIFEST references is gone. Reporting NotFound would let
    // Get answer "no such key" and hide the data loss.
    return Status::Corruption(file_name + ": table referenced by the MANIFEST is missing");
  }
  STRATA_RETURN_IF_ERROR(file.status());
  STRATA_ASSIGN_OR_RETURN(
      auto opened, table::Table::Open({.comparator = comparator_}, std::move(*file), file_size));
  std::shared_ptr<const table::Table> table = std::move(opened);
  const std::lock_guard<std::mutex> guard(mutex_);
  return tables_.try_emplace(number, std::move(table)).first->second;
}

void TableCache::Evict(std::uint64_t number) {
  const std::lock_guard<std::mutex> guard(mutex_);
  tables_.erase(number);
}

}  // namespace strata

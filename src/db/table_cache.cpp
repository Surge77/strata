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
  STRATA_ASSIGN_OR_RETURN(auto file, env_->NewRandomAccessFile(TableFileName(dbname_, number)));
  STRATA_ASSIGN_OR_RETURN(
      auto opened, table::Table::Open({.comparator = comparator_}, std::move(file), file_size));
  std::shared_ptr<const table::Table> table = std::move(opened);
  const std::lock_guard<std::mutex> guard(mutex_);
  return tables_.try_emplace(number, std::move(table)).first->second;
}

void TableCache::Evict(std::uint64_t number) {
  const std::lock_guard<std::mutex> guard(mutex_);
  tables_.erase(number);
}

}  // namespace strata

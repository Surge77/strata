#include "db/table_cache.hpp"

#include "db/filename.hpp"

namespace strata {
namespace {

// A table's iterator together with the table it reads.
class TableHoldingIterator final : public Iterator {
 public:
  explicit TableHoldingIterator(std::shared_ptr<const table::Table> table)
      : table_(std::move(table)), it_(table_->NewIterator()) {}

  [[nodiscard]] bool Valid() const override { return it_->Valid(); }
  void SeekToFirst() override { it_->SeekToFirst(); }
  void SeekToLast() override { it_->SeekToLast(); }
  void Seek(Slice target) override { it_->Seek(target); }
  void Next() override { it_->Next(); }
  void Prev() override { it_->Prev(); }
  [[nodiscard]] Slice key() const override { return it_->key(); }
  [[nodiscard]] Slice value() const override { return it_->value(); }
  [[nodiscard]] Status status() const override { return it_->status(); }

 private:
  std::shared_ptr<const table::Table> table_;  // outlives it_, declared first
  std::unique_ptr<Iterator> it_;
};

}  // namespace

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
  STRATA_ASSIGN_OR_RETURN(auto opened, table::Table::Open({.comparator = comparator_,
                                                           .block_cache = block_cache_.get(),
                                                           .cache_id = number},
                                                          std::move(*file), file_size));
  std::shared_ptr<const table::Table> table = std::move(opened);
  return tables_.Insert(number, std::move(table), kTableCharge);
}

// The deleted table's cached blocks are left to age out of the block cache:
// its id is never reused, so they can never be served again.
void TableCache::Evict(std::uint64_t number) {
  tables_.Erase(number);
}

Result<std::unique_ptr<Iterator>> TableCache::NewIterator(std::uint64_t number,
                                                          std::uint64_t file_size) {
  STRATA_ASSIGN_OR_RETURN(auto table, Find(number, file_size));
  return std::unique_ptr<Iterator>(std::make_unique<TableHoldingIterator>(std::move(table)));
}

}  // namespace strata

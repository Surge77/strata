#include "table/table.hpp"

#include <string>
#include <utility>

#include "table/bloom_filter.hpp"

namespace strata::table {
namespace {

// An iterator over a block that shares ownership of the block, so data-block
// iterators keep their bytes alive even after the block cache evicts them.
class OwningBlockIterator final : public Iterator {
 public:
  OwningBlockIterator(std::shared_ptr<const Block> block, const Comparator* comparator)
      : block_(std::move(block)), it_(block_->NewIterator(comparator)) {}

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
  std::shared_ptr<const Block> block_;
  std::unique_ptr<Iterator> it_;
};

// Walks the index block and, inside each index entry, the data block it
// points to. Empty data blocks are skipped. The first block that fails to
// load or parse ends the iteration: Valid() becomes false and status()
// reports the error.
class TwoLevelIterator final : public Iterator {
 public:
  using BlockLoader = std::function<Result<std::unique_ptr<Iterator>>(Slice index_value)>;

  TwoLevelIterator(std::unique_ptr<Iterator> index, BlockLoader loader)
      : index_(std::move(index)), loader_(std::move(loader)) {}

  [[nodiscard]] bool Valid() const override {
    return !failed_ && data_ != nullptr && data_->Valid();
  }

  void SeekToFirst() override {
    Reset();
    index_->SeekToFirst();
    LoadBlock();
    if (data_ != nullptr) {
      data_->SeekToFirst();
    }
    SkipEmptyBlocksForward();
  }

  void SeekToLast() override {
    Reset();
    index_->SeekToLast();
    LoadBlock();
    if (data_ != nullptr) {
      data_->SeekToLast();
    }
    SkipEmptyBlocksBackward();
  }

  void Seek(Slice target) override {
    Reset();
    index_->Seek(target);
    LoadBlock();
    if (data_ != nullptr) {
      data_->Seek(target);
    }
    SkipEmptyBlocksForward();
  }

  void Next() override {
    data_->Next();
    SkipEmptyBlocksForward();
  }

  void Prev() override {
    data_->Prev();
    SkipEmptyBlocksBackward();
  }

  [[nodiscard]] Slice key() const override { return data_->key(); }
  [[nodiscard]] Slice value() const override { return data_->value(); }

  [[nodiscard]] Status status() const override {
    if (!index_->status().ok()) {
      return index_->status();
    }
    if (data_ != nullptr && !data_->status().ok()) {
      return data_->status();
    }
    return status_;
  }

 private:
  void Reset() {
    failed_ = false;
    status_ = Status::Ok();
    data_ = nullptr;
    loaded_handle_.clear();
  }

  // Makes data_ iterate the block the index currently points to.
  void LoadBlock() {
    if (!index_->Valid()) {
      data_ = nullptr;
      if (!index_->status().ok()) {
        failed_ = true;
      }
      return;
    }
    const Slice handle = index_->value();
    if (data_ != nullptr && handle == loaded_handle_) {
      return;
    }
    auto block = loader_(handle);
    if (!block.ok()) {
      failed_ = true;
      status_ = block.status();
      data_ = nullptr;
      return;
    }
    data_ = std::move(block).value();
    loaded_handle_.assign(handle);
  }

  // Stops on failure: a data block that reports a parse error ends iteration.
  bool DataFailed() {
    if (data_ != nullptr && !data_->status().ok()) {
      failed_ = true;
      return true;
    }
    return failed_;
  }

  void SkipEmptyBlocksForward() {
    while (!DataFailed() && (data_ == nullptr || !data_->Valid())) {
      if (!index_->Valid()) {
        data_ = nullptr;
        return;
      }
      index_->Next();
      LoadBlock();
      if (data_ != nullptr) {
        data_->SeekToFirst();
      }
    }
  }

  void SkipEmptyBlocksBackward() {
    while (!DataFailed() && (data_ == nullptr || !data_->Valid())) {
      if (!index_->Valid()) {
        data_ = nullptr;
        return;
      }
      index_->Prev();
      LoadBlock();
      if (data_ != nullptr) {
        data_->SeekToLast();
      }
    }
  }

  std::unique_ptr<Iterator> index_;
  BlockLoader loader_;
  std::unique_ptr<Iterator> data_;
  std::string loaded_handle_;
  bool failed_ = false;
  Status status_;
};

Result<std::unique_ptr<Block>> ReadAndParseBlock(const RandomAccessFile& file,
                                                 std::uint64_t file_size,
                                                 const BlockHandle& handle) {
  STRATA_ASSIGN_OR_RETURN(std::string contents, ReadBlock(file, file_size, handle));
  return Block::Create(std::move(contents));
}

}  // namespace

Result<std::unique_ptr<Table>> Table::Open(const TableOptions& options,
                                           std::unique_ptr<RandomAccessFile> file,
                                           std::uint64_t file_size) {
  if (file_size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an SSTable");
  }
  std::string footer_space(Footer::kEncodedLength, '\0');
  STRATA_ASSIGN_OR_RETURN(
      const Slice footer_input,
      file->Read(file_size - Footer::kEncodedLength, Footer::kEncodedLength, footer_space.data()));
  Footer footer;
  STRATA_RETURN_IF_ERROR(footer.DecodeFrom(footer_input));

  STRATA_ASSIGN_OR_RETURN(auto index, ReadAndParseBlock(*file, file_size, footer.index_handle));
  STRATA_ASSIGN_OR_RETURN(auto metaindex,
                          ReadAndParseBlock(*file, file_size, footer.metaindex_handle));
  auto meta = metaindex->NewIterator(BytewiseComparator());
  meta->Seek(kUserBloomFilterMetaName);
  std::optional<std::string> filter;
  if (meta->Valid() && meta->key() == kUserBloomFilterMetaName) {
    BlockHandle handle;
    Slice encoded_handle = meta->value();
    STRATA_RETURN_IF_ERROR(handle.DecodeFrom(&encoded_handle));
    if (!encoded_handle.empty()) {
      return Status::Corruption("filter block handle has trailing bytes");
    }
    STRATA_ASSIGN_OR_RETURN(std::string contents, ReadBlock(*file, file_size, handle));
    STRATA_RETURN_IF_ERROR(BloomFilter::Validate(contents));
    filter = std::move(contents);
  }
  STRATA_RETURN_IF_ERROR(meta->status());
  if (!BloomFilter::SupportsComparator(options.comparator)) {
    filter.reset();
  }

  return std::unique_ptr<Table>(new Table(options, std::move(file), file_size, std::move(index),
                                          footer.metaindex_handle.offset(), std::move(filter)));
}

Result<std::unique_ptr<Iterator>> Table::BlockIterator(Slice index_value) const {
  BlockHandle handle;
  Slice input = index_value;
  STRATA_RETURN_IF_ERROR(handle.DecodeFrom(&input));
  STRATA_ASSIGN_OR_RETURN(auto block, ReadDataBlock(handle));
  return std::unique_ptr<Iterator>(
      std::make_unique<OwningBlockIterator>(std::move(block), options_.comparator));
}

Result<std::shared_ptr<const Block>> Table::ReadDataBlock(const BlockHandle& handle) const {
  BlockCache* const cache = options_.block_cache;
  const BlockCacheKey key{.cache_id = options_.cache_id, .offset = handle.offset()};
  if (cache != nullptr) {
    if (auto cached = cache->Lookup(key)) {
      return cached;
    }
  }
  STRATA_ASSIGN_OR_RETURN(auto parsed, ReadAndParseBlock(*file_, file_size_, handle));
  std::shared_ptr<const Block> block = std::move(parsed);
  if (cache == nullptr) {
    return block;
  }
  const std::size_t charge = block->size();
  return cache->Insert(key, std::move(block), charge);
}

std::unique_ptr<Iterator> Table::NewIterator() const {
  return std::make_unique<TwoLevelIterator>(
      index_->NewIterator(options_.comparator),
      [this](Slice index_value) { return BlockIterator(index_value); });
}

// Goes through the two-level iterator rather than searching one block: index
// separators are shortened, so a target can fall between a block's last key
// and its separator, and the answer is then the *next* block's first key.
Status Table::Seek(Slice target, const std::function<void(Slice key, Slice value)>& found) const {
  auto it = NewIterator();
  it->Seek(target);
  if (it->Valid()) {
    found(it->key(), it->value());
  }
  return it->status();
}

bool Table::MayContainUserKey(Slice user_key) const noexcept {
  return !filter_ || BloomFilter::MayContain(user_key, *filter_);
}

std::uint64_t Table::ApproximateOffsetOf(Slice key) const {
  auto index = index_->NewIterator(options_.comparator);
  index->Seek(key);
  if (index->Valid()) {
    BlockHandle handle;
    Slice input = index->value();
    if (handle.DecodeFrom(&input).ok()) {
      return handle.offset();
    }
  }
  // Past the last key (or an unreadable index entry): the data ends where the
  // metaindex begins.
  return metaindex_offset_;
}

}  // namespace strata::table

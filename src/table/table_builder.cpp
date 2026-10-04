#include "table/table_builder.hpp"

#include <cassert>

#include "db/dbformat.hpp"
#include "table/bloom_filter.hpp"
#include "util/coding.hpp"
#include "util/crc32c.hpp"

namespace strata::table {
namespace {

// Index blocks are searched once per lookup and never scanned; a restart at
// every entry makes that search a pure binary search.
constexpr int kIndexRestartInterval = 1;

}  // namespace

TableBuilder::TableBuilder(const TableOptions& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.comparator, options.block_restart_interval),
      index_block_(options.comparator, kIndexRestartInterval) {
  assert(options_.comparator != nullptr);
  if (options_.use_filter && !BloomFilter::SupportsComparator(options_.comparator)) {
    status_ = Status::InvalidArgument("Bloom filter requires bytewise internal-key comparator");
  }
}

Status TableBuilder::Add(Slice key, Slice value) {
  assert(!closed_);
  STRATA_RETURN_IF_ERROR(status_);
  if (num_entries_ > 0 && options_.comparator->Compare(key, last_key_) <= 0) {
    status_ = Status::InvalidArgument("keys must be added to a table in strictly increasing order");
    return status_;
  }
  if (options_.use_filter) {
    ParsedInternalKey parsed;
    if (!ParseInternalKey(key, &parsed)) {
      status_ = Status::InvalidArgument("filtered table requires internal keys");
      return status_;
    }
    filter_hashes_.push_back(BloomFilter::Hash(parsed.user_key));
  }

  if (pending_index_entry_) {
    // The previous block's separator: >= its last key, < this key.
    options_.comparator->FindShortestSeparator(&last_key_, key);
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(last_key_, handle_encoding);
    pending_index_entry_ = false;
  }

  last_key_.assign(key);
  ++num_entries_;
  data_block_.Add(key, value);
  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    FlushDataBlock();
  }
  return status_;
}

void TableBuilder::FlushDataBlock() {
  if (!status_.ok() || data_block_.empty()) {
    return;
  }
  assert(!pending_index_entry_);
  status_ = WriteBlock(&data_block_, &pending_handle_);
  if (status_.ok()) {
    pending_index_entry_ = true;
  }
}

Status TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  const Slice contents = block->Finish();
  Status status = WriteRawBlock(contents, handle);
  block->Reset();
  return status;
}

Status TableBuilder::WriteRawBlock(Slice contents, BlockHandle* handle) {
  *handle = BlockHandle(offset_, contents.size());
  STRATA_RETURN_IF_ERROR(file_->Append(contents));

  std::string trailer(1, static_cast<char>(CompressionType::kNone));
  std::uint32_t crc = crc32c::Value(contents);
  crc = crc32c::Extend(crc, trailer);  // the checksum covers the type byte too
  PutFixed32(&trailer, crc32c::Mask(crc));
  STRATA_RETURN_IF_ERROR(file_->Append(trailer));

  offset_ += contents.size() + kBlockTrailerSize;
  return Status::Ok();
}

Status TableBuilder::Finish() {
  assert(!closed_);
  FlushDataBlock();
  closed_ = true;
  STRATA_RETURN_IF_ERROR(status_);

  BlockBuilder metaindex_block(BytewiseComparator(), kIndexRestartInterval);
  if (options_.use_filter) {
    const std::string filter = BloomFilter::Build(filter_hashes_);
    BlockHandle filter_handle;
    status_ = WriteRawBlock(filter, &filter_handle);
    STRATA_RETURN_IF_ERROR(status_);
    std::string handle_encoding;
    filter_handle.EncodeTo(&handle_encoding);
    metaindex_block.Add(kUserBloomFilterMetaName, handle_encoding);
  }
  BlockHandle metaindex_handle;
  status_ = WriteBlock(&metaindex_block, &metaindex_handle);
  STRATA_RETURN_IF_ERROR(status_);

  if (pending_index_entry_) {
    // The last block has no successor, so its separator only needs to be >=
    // its last key.
    options_.comparator->FindShortSuccessor(&last_key_);
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(last_key_, handle_encoding);
    pending_index_entry_ = false;
  }
  BlockHandle index_handle;
  status_ = WriteBlock(&index_block_, &index_handle);
  STRATA_RETURN_IF_ERROR(status_);

  Footer footer;
  footer.metaindex_handle = metaindex_handle;
  footer.index_handle = index_handle;
  std::string footer_encoding;
  footer.EncodeTo(&footer_encoding);
  status_ = file_->Append(footer_encoding);
  if (status_.ok()) {
    offset_ += footer_encoding.size();
  }
  return status_;
}

}  // namespace strata::table

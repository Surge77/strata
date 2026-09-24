// WriteBatch encoding; the layout is specified in docs/format/write_batch.md.

#include "strata/write_batch.hpp"

#include "db/dbformat.hpp"
#include "db/write_batch_internal.hpp"
#include "util/coding.hpp"

namespace strata {

WriteBatch::WriteBatch() {
  Clear();
}

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(WriteBatchInternal::kHeaderSize);
  oversized_ = false;
}

std::uint32_t WriteBatch::Count() const noexcept {
  return WriteBatchInternal::Count(*this);
}

void WriteBatch::Put(Slice key, Slice value) {
  if (key.size() > kMaxKeySize || value.size() > kMaxValueSize) {
    oversized_ = true;
    return;
  }
  WriteBatchInternal::SetCount(this, Count() + 1);
  rep_.push_back(static_cast<char>(ValueType::kValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(Slice key) {
  if (key.size() > kMaxKeySize) {
    oversized_ = true;
    return;
  }
  WriteBatchInternal::SetCount(this, Count() + 1);
  rep_.push_back(static_cast<char>(ValueType::kDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

void WriteBatch::Append(const WriteBatch& other) {
  oversized_ = oversized_ || other.oversized_;
  // The count is a fixed32; a combined batch past 2^32 - 1 operations cannot
  // be encoded, so it is refused like any other oversized batch.
  if (std::uint64_t{Count()} + other.Count() > UINT32_MAX) {
    oversized_ = true;
    return;
  }
  WriteBatchInternal::SetCount(this, Count() + other.Count());
  rep_.append(other.rep_, WriteBatchInternal::kHeaderSize);
}

Status WriteBatch::Iterate(Handler* handler) const {
  // rep_ always holds at least a header: the constructor and Clear create one,
  // and SetContents rejects anything shorter.
  Slice input(rep_);
  input.remove_prefix(WriteBatchInternal::kHeaderSize);

  const std::uint32_t expected = Count();
  std::uint32_t found = 0;
  while (!input.empty()) {
    if (found == expected) {
      return Status::Corruption("write batch has bytes after its last operation");
    }
    const auto tag = static_cast<std::uint8_t>(input.front());
    input.remove_prefix(1);

    Slice key;
    Slice value;
    if (tag == static_cast<std::uint8_t>(ValueType::kValue)) {
      if (!GetLengthPrefixedSlice(&input, &key) || !GetLengthPrefixedSlice(&input, &value)) {
        return Status::Corruption("write batch has a malformed Put");
      }
      handler->Put(key, value);
    } else if (tag == static_cast<std::uint8_t>(ValueType::kDeletion)) {
      if (!GetLengthPrefixedSlice(&input, &key)) {
        return Status::Corruption("write batch has a malformed Delete");
      }
      handler->Delete(key);
    } else {
      return Status::Corruption("write batch has an unknown operation tag");
    }
    ++found;
  }
  if (found != expected) {
    return Status::Corruption("write batch count does not match its operations");
  }
  return Status::Ok();
}

std::uint32_t WriteBatchInternal::Count(const WriteBatch& batch) noexcept {
  return DecodeFixed32(batch.rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch* batch, std::uint32_t count) noexcept {
  EncodeFixed32(batch->rep_.data() + 8, count);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch& batch) noexcept {
  return DecodeFixed64(batch.rep_.data());
}

void WriteBatchInternal::SetSequence(WriteBatch* batch, SequenceNumber sequence) noexcept {
  EncodeFixed64(batch->rep_.data(), sequence);
}

Status WriteBatchInternal::SetContents(WriteBatch* batch, Slice contents) {
  if (contents.size() < kHeaderSize) {
    return Status::Corruption("write batch shorter than its header");
  }
  batch->rep_.assign(contents);
  return Status::Ok();
}

}  // namespace strata

#include "table/block.hpp"

#include <string>

#include "util/coding.hpp"

namespace strata::table {
namespace {

struct EntryHeader {
  std::uint32_t shared = 0;
  std::uint32_t non_shared = 0;
  std::uint32_t value_length = 0;
  std::size_t header_size = 0;  // bytes the three varints occupied
};

// Decodes the header of the entry at `offset`, checking that the header and
// the key delta and value it announces all lie before `limit`. False on any
// violation.
bool DecodeEntryHeader(const char* data, std::uint32_t offset, std::uint32_t limit,
                       EntryHeader* header) {
  if (offset >= limit) {
    return false;
  }
  Slice input(data + offset, limit - offset);
  const std::size_t available = input.size();
  if (!GetVarint32(&input, &header->shared) || !GetVarint32(&input, &header->non_shared) ||
      !GetVarint32(&input, &header->value_length)) {
    return false;
  }
  header->header_size = available - input.size();
  // 64-bit sum: two 32-bit lengths cannot overflow it.
  return std::uint64_t{header->non_shared} + header->value_length <= input.size();
}

class BlockIterator final : public Iterator {
 public:
  BlockIterator(const Comparator* comparator, const char* data, std::uint32_t restarts,
                std::uint32_t num_restarts)
      : comparator_(comparator),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(restarts),
        restart_index_(num_restarts) {}

  [[nodiscard]] bool Valid() const override { return current_ < restarts_; }
  [[nodiscard]] Status status() const override { return status_; }
  [[nodiscard]] Slice key() const override { return key_; }
  [[nodiscard]] Slice value() const override { return value_; }

  void Next() override { ParseNextKey(); }

  // Entries only link forward, so Prev backs up to the restart point before
  // the current entry and scans forward to the entry just before it.
  void Prev() override {
    const std::uint32_t original = current_;
    while (RestartPoint(restart_index_) >= original) {
      if (restart_index_ == 0) {
        MarkInvalid();
        return;
      }
      --restart_index_;
    }
    SeekToRestartPoint(restart_index_);
    while (ParseNextKey() && NextEntryOffset() < original) {
    }
  }

  // Binary search over restart points (whose keys are stored whole) for the
  // last one before `target`, then a linear scan of at most one interval.
  void Seek(Slice target) override {
    std::uint32_t left = 0;
    std::uint32_t right = num_restarts_ - 1;
    while (left < right) {
      const std::uint32_t mid = left + (right - left + 1) / 2;
      EntryHeader header;
      const std::uint32_t offset = RestartPoint(mid);
      if (!DecodeEntryHeader(data_, offset, restarts_, &header) || header.shared != 0) {
        CorruptionError();
        return;
      }
      const Slice mid_key(data_ + offset + header.header_size, header.non_shared);
      if (comparator_->Compare(mid_key, target) < 0) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }
    SeekToRestartPoint(left);
    while (ParseNextKey()) {
      if (comparator_->Compare(key_, target) >= 0) {
        return;
      }
    }
  }

  void SeekToFirst() override {
    SeekToRestartPoint(0);
    ParseNextKey();
  }

  void SeekToLast() override {
    SeekToRestartPoint(num_restarts_ - 1);
    while (ParseNextKey() && NextEntryOffset() < restarts_) {
    }
  }

 private:
  [[nodiscard]] std::uint32_t RestartPoint(std::uint32_t index) const {
    return DecodeFixed32(data_ + restarts_ + index * sizeof(std::uint32_t));
  }

  // Offset just past the current entry: where the next one begins.
  [[nodiscard]] std::uint32_t NextEntryOffset() const {
    return static_cast<std::uint32_t>(value_.data() + value_.size() - data_);
  }

  void SeekToRestartPoint(std::uint32_t index) {
    key_.clear();
    restart_index_ = index;
    // ParseNextKey starts from NextEntryOffset(), i.e. the end of value_.
    value_ = Slice(data_ + RestartPoint(index), 0);
  }

  void MarkInvalid() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
  }

  void CorruptionError() {
    MarkInvalid();
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_ = Slice();
  }

  bool ParseNextKey() {
    current_ = NextEntryOffset();
    if (current_ >= restarts_) {
      MarkInvalid();
      return false;
    }
    EntryHeader header;
    if (!DecodeEntryHeader(data_, current_, restarts_, &header) || header.shared > key_.size()) {
      CorruptionError();
      return false;
    }
    const char* const key_delta = data_ + current_ + header.header_size;
    key_.resize(header.shared);
    key_.append(key_delta, header.non_shared);
    value_ = Slice(key_delta + header.non_shared, header.value_length);
    while (restart_index_ + 1 < num_restarts_ && RestartPoint(restart_index_ + 1) < current_) {
      ++restart_index_;
    }
    return true;
  }

  const Comparator* const comparator_;
  const char* const data_;
  const std::uint32_t restarts_;      // offset of the restart array
  const std::uint32_t num_restarts_;  // >= 1 (checked by Block::Create)
  std::uint32_t current_;             // offset of the current entry; == restarts_ if invalid
  std::uint32_t restart_index_;       // restart block containing current_
  std::string key_;
  Slice value_;
  Status status_;
};

}  // namespace

Result<std::unique_ptr<Block>> Block::Create(std::string contents) {
  constexpr std::size_t kWord = sizeof(std::uint32_t);
  if (contents.size() < kWord || contents.size() > UINT32_MAX) {
    return Status::Corruption("block has an impossible size");
  }
  const std::uint32_t num_restarts = DecodeFixed32(contents.data() + contents.size() - kWord);
  const std::size_t max_restarts = (contents.size() - kWord) / kWord;
  if (num_restarts == 0 || num_restarts > max_restarts) {
    return Status::Corruption("block restart count is out of range");
  }
  const auto restart_offset =
      static_cast<std::uint32_t>(contents.size() - (std::size_t{1} + num_restarts) * kWord);
  // Every restart point must lie inside the entry region, so iterators can
  // use them without re-checking.
  for (std::uint32_t index = 0; index < num_restarts; ++index) {
    const std::uint32_t restart = DecodeFixed32(contents.data() + restart_offset + index * kWord);
    if (restart > restart_offset) {
      return Status::Corruption("block restart point is past the entries");
    }
  }
  return std::unique_ptr<Block>(new Block(std::move(contents), restart_offset, num_restarts));
}

std::unique_ptr<Iterator> Block::NewIterator(const Comparator* comparator) const {
  return std::make_unique<BlockIterator>(comparator, data_.data(), restart_offset_, num_restarts_);
}

}  // namespace strata::table

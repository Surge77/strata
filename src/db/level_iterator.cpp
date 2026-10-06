#include "db/level_iterator.hpp"

#include <algorithm>
#include <utility>

namespace strata {
namespace {

class LevelIterator final : public Iterator {
 public:
  LevelIterator(const InternalKeyComparator* comparator, TableCache* tables,
                const std::vector<FileMetaData>* files)
      : comparator_(comparator), tables_(tables), files_(*files) {}

  [[nodiscard]] bool Valid() const override {
    return status_.ok() && data_ != nullptr && data_->Valid();
  }

  void Seek(Slice target) override {
    // The first table whose largest key is at or after the target.
    const auto found = std::lower_bound(files_.begin(), files_.end(), target,
                                        [this](const FileMetaData& file, Slice key) {
                                          return comparator_->Compare(file.largest, key) < 0;
                                        });
    if (!Open(static_cast<std::size_t>(found - files_.begin()))) {
      return;
    }
    data_->Seek(target);
    SkipEmptyForward();
  }

  void SeekToFirst() override {
    if (!Open(0)) {
      return;
    }
    data_->SeekToFirst();
    SkipEmptyForward();
  }

  void SeekToLast() override {
    if (files_.empty() || !Open(files_.size() - 1)) {
      Clear();
      return;
    }
    data_->SeekToLast();
    SkipEmptyBackward();
  }

  void Next() override {
    data_->Next();
    SkipEmptyForward();
  }

  void Prev() override {
    data_->Prev();
    SkipEmptyBackward();
  }

  [[nodiscard]] Slice key() const override { return data_->key(); }
  [[nodiscard]] Slice value() const override { return data_->value(); }

  [[nodiscard]] Status status() const override {
    if (!status_.ok()) {
      return status_;
    }
    return data_ != nullptr ? data_->status() : Status::Ok();
  }

 private:
  void Clear() {
    data_ = nullptr;
    index_ = files_.size();
  }

  // Makes data_ iterate table `index` (past the end: no table). False once
  // the iteration has failed or run off the end.
  bool Open(std::size_t index) {
    status_ = Status::Ok();
    if (index >= files_.size()) {
      Clear();
      return false;
    }
    if (data_ != nullptr && index_ == index) {
      return true;
    }
    auto opened = tables_->NewIterator(files_[index].number, files_[index].file_size);
    if (!opened.ok()) {
      status_ = opened.status();
      Clear();
      return false;
    }
    data_ = std::move(opened).value();
    index_ = index;
    return true;
  }

  // A table that fails to read stops the scan rather than being skipped.
  bool Failed() const { return !status_.ok() || (data_ != nullptr && !data_->status().ok()); }

  void SkipEmptyForward() {
    while (data_ != nullptr && !data_->Valid() && !Failed()) {
      if (!Open(index_ + 1)) {
        return;
      }
      data_->SeekToFirst();
    }
  }

  void SkipEmptyBackward() {
    while (data_ != nullptr && !data_->Valid() && !Failed()) {
      if (index_ == 0) {
        Clear();
        return;
      }
      if (!Open(index_ - 1)) {
        return;
      }
      data_->SeekToLast();
    }
  }

  const InternalKeyComparator* const comparator_;
  TableCache* const tables_;
  const std::vector<FileMetaData>& files_;
  std::unique_ptr<Iterator> data_;
  std::size_t index_ = 0;
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewLevelIterator(const InternalKeyComparator* comparator,
                                           TableCache* tables,
                                           const std::vector<FileMetaData>* files) {
  return std::make_unique<LevelIterator>(comparator, tables, files);
}

}  // namespace strata

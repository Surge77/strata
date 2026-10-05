#include "table/merger.hpp"

#include <string>
#include <utility>

namespace strata::table {
namespace {

// Picks the smallest (forward) or largest (reverse) current child by a linear
// scan. Compaction merges a handful of tables, so a heap would not pay off.
class MergingIterator final : public Iterator {
 public:
  MergingIterator(const Comparator* comparator, std::vector<std::unique_ptr<Iterator>> children)
      : comparator_(comparator), children_(std::move(children)) {}

  [[nodiscard]] bool Valid() const override { return current_ != nullptr; }

  void SeekToFirst() override {
    for (auto& child : children_) {
      child->SeekToFirst();
    }
    direction_ = Direction::kForward;
    FindSmallest();
  }

  void SeekToLast() override {
    for (auto& child : children_) {
      child->SeekToLast();
    }
    direction_ = Direction::kReverse;
    FindLargest();
  }

  void Seek(Slice target) override {
    for (auto& child : children_) {
      child->Seek(target);
    }
    direction_ = Direction::kForward;
    FindSmallest();
  }

  void Next() override {
    if (direction_ != Direction::kForward) {
      // Every other child sits before key(); move each to the first entry
      // after it.
      const std::string current_key(key());
      for (auto& child : children_) {
        if (child.get() == current_) {
          continue;
        }
        child->Seek(current_key);
        if (child->Valid() && comparator_->Compare(current_key, child->key()) == 0) {
          child->Next();
        }
      }
      direction_ = Direction::kForward;
    }
    current_->Next();
    FindSmallest();
  }

  void Prev() override {
    if (direction_ != Direction::kReverse) {
      // Every other child sits at or after key(); move each to the last entry
      // before it.
      const std::string current_key(key());
      for (auto& child : children_) {
        if (child.get() == current_) {
          continue;
        }
        child->Seek(current_key);
        if (child->Valid()) {
          child->Prev();
        } else if (child->status().ok()) {
          child->SeekToLast();
        }
      }
      direction_ = Direction::kReverse;
    }
    current_->Prev();
    FindLargest();
  }

  [[nodiscard]] Slice key() const override { return current_->key(); }
  [[nodiscard]] Slice value() const override { return current_->value(); }

  [[nodiscard]] Status status() const override {
    for (const auto& child : children_) {
      if (Status status = child->status(); !status.ok()) {
        return status;
      }
    }
    return Status::Ok();
  }

 private:
  enum class Direction { kForward, kReverse };

  // False if any child has failed: the merge must stop rather than skip it.
  bool AllChildrenHealthy() const {
    for (const auto& child : children_) {
      if (!child->Valid() && !child->status().ok()) {
        return false;
      }
    }
    return true;
  }

  void FindSmallest() {
    current_ = nullptr;
    if (!AllChildrenHealthy()) {
      return;
    }
    for (auto& child : children_) {
      if (child->Valid() &&
          (current_ == nullptr || comparator_->Compare(child->key(), current_->key()) < 0)) {
        current_ = child.get();
      }
    }
  }

  void FindLargest() {
    current_ = nullptr;
    if (!AllChildrenHealthy()) {
      return;
    }
    for (auto& child : children_) {
      if (child->Valid() &&
          (current_ == nullptr || comparator_->Compare(child->key(), current_->key()) > 0)) {
        current_ = child.get();
      }
    }
  }

  const Comparator* const comparator_;
  std::vector<std::unique_ptr<Iterator>> children_;
  Iterator* current_ = nullptr;
  Direction direction_ = Direction::kForward;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(const Comparator* comparator,
                                             std::vector<std::unique_ptr<Iterator>> children) {
  return std::make_unique<MergingIterator>(comparator, std::move(children));
}

}  // namespace strata::table

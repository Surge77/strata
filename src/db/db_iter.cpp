#include "db/db_iter.hpp"

#include <utility>

namespace strata {
namespace {

// The state machine is LevelDB's DBIter, with bounds added.
//
// Moving forward, the internal iterator sits exactly on the entry being shown,
// and key() / value() read straight from it. Moving backward it sits just
// before every entry of the shown key, so that key and its value are copied
// into saved_key_ / saved_value_. Next and Prev switch between the two.
class DBIter final : public Iterator {
 public:
  DBIter(const Comparator* user_comparator, std::unique_ptr<Iterator> internal,
         SequenceNumber sequence, DBIterBounds bounds,
         std::vector<std::shared_ptr<const void>> keep_alive)
      : user_comparator_(user_comparator),
        keep_alive_(std::move(keep_alive)),
        iter_(std::move(internal)),
        sequence_(sequence),
        bounds_(std::move(bounds)) {}

  [[nodiscard]] bool Valid() const override { return valid_; }

  [[nodiscard]] Slice key() const override {
    return direction_ == Direction::kForward ? ExtractUserKey(iter_->key()) : Slice(saved_key_);
  }
  [[nodiscard]] Slice value() const override {
    return direction_ == Direction::kForward ? iter_->value() : Slice(saved_value_);
  }
  [[nodiscard]] Status status() const override { return status_.ok() ? iter_->status() : status_; }

  void Next() override {
    if (direction_ == Direction::kReverse) {
      // The internal iterator is just before key()'s entries: step into them,
      // then skip past them below, with saved_key_ already holding key().
      direction_ = Direction::kForward;
      if (!iter_->Valid()) {
        iter_->SeekToFirst();
      } else {
        iter_->Next();
      }
      if (!iter_->Valid()) {
        Invalidate();
        return;
      }
    } else {
      saved_key_.assign(ExtractUserKey(iter_->key()));
      iter_->Next();
      if (!iter_->Valid()) {
        Invalidate();
        return;
      }
    }
    FindNextUserEntry(/*skipping=*/true);
    EnforceUpperBound();
  }

  void Prev() override {
    if (direction_ == Direction::kForward) {
      // Walk back past every entry of the current key.
      saved_key_.assign(ExtractUserKey(iter_->key()));
      while (true) {
        iter_->Prev();
        if (!iter_->Valid()) {
          Invalidate();
          return;
        }
        if (user_comparator_->Compare(ExtractUserKey(iter_->key()), saved_key_) < 0) {
          break;
        }
      }
      direction_ = Direction::kReverse;
    }
    FindPrevUserEntry();
    EnforceLowerBound();
  }

  void Seek(Slice target) override {
    direction_ = Direction::kForward;
    saved_value_.clear();
    Slice start = target;
    if (bounds_.lower && user_comparator_->Compare(start, *bounds_.lower) < 0) {
      start = *bounds_.lower;
    }
    saved_key_.clear();
    AppendInternalKey(&saved_key_, ParsedInternalKey{start, sequence_, kValueTypeForSeek});
    iter_->Seek(saved_key_);
    if (!iter_->Valid()) {
      Invalidate();
      return;
    }
    FindNextUserEntry(/*skipping=*/false);
    EnforceUpperBound();
  }

  void SeekToFirst() override {
    if (bounds_.lower) {
      Seek(*bounds_.lower);
      return;
    }
    direction_ = Direction::kForward;
    saved_value_.clear();
    iter_->SeekToFirst();
    if (!iter_->Valid()) {
      Invalidate();
      return;
    }
    FindNextUserEntry(/*skipping=*/false);
    EnforceUpperBound();
  }

  void SeekToLast() override {
    direction_ = Direction::kReverse;
    saved_value_.clear();
    if (bounds_.upper) {
      // Every entry of a key below the upper bound sorts before the first
      // entry of the bound itself.
      std::string bound;
      AppendInternalKey(&bound,
                        ParsedInternalKey{*bounds_.upper, kMaxSequenceNumber, kValueTypeForSeek});
      iter_->Seek(bound);
      if (iter_->Valid()) {
        iter_->Prev();
      } else {
        iter_->SeekToLast();
      }
    } else {
      iter_->SeekToLast();
    }
    FindPrevUserEntry();
    EnforceLowerBound();
  }

 private:
  enum class Direction { kForward, kReverse };

  void Invalidate() {
    valid_ = false;
    saved_key_.clear();
    saved_value_.clear();
  }

  // Parses the current internal key; a malformed one ends the scan.
  bool ParseCurrent(ParsedInternalKey* key) {
    if (ParseInternalKey(iter_->key(), key)) {
      return true;
    }
    status_ = Status::Corruption("iterator met a malformed internal key");
    Invalidate();
    return false;
  }

  // From the current entry forward, the first visible, live user key. With
  // `skipping`, entries for user keys at or below saved_key_ are passed over.
  void FindNextUserEntry(bool skipping) {
    do {
      ParsedInternalKey parsed;
      if (!ParseCurrent(&parsed)) {
        return;
      }
      if (parsed.sequence <= sequence_) {
        if (parsed.type == ValueType::kDeletion) {
          // Hide every older version of this key.
          saved_key_.assign(parsed.user_key);
          skipping = true;
        } else if (!skipping || user_comparator_->Compare(parsed.user_key, saved_key_) > 0) {
          valid_ = true;
          saved_key_.clear();
          return;
        }
      }
      iter_->Next();
    } while (iter_->Valid());
    Invalidate();
  }

  // From the current entry backward, the previous visible, live user key,
  // copied into saved_key_ / saved_value_. Leaves the internal iterator just
  // before that key's entries.
  void FindPrevUserEntry() {
    ValueType value_type = ValueType::kDeletion;
    if (iter_->Valid()) {
      do {
        ParsedInternalKey parsed;
        if (!ParseCurrent(&parsed)) {
          return;
        }
        if (parsed.sequence <= sequence_) {
          if (value_type != ValueType::kDeletion &&
              user_comparator_->Compare(parsed.user_key, saved_key_) < 0) {
            break;  // saved_key_ holds a live value, and this is an earlier key
          }
          value_type = parsed.type;
          if (value_type == ValueType::kDeletion) {
            saved_key_.clear();
            saved_value_.clear();
          } else {
            saved_key_.assign(parsed.user_key);
            saved_value_.assign(iter_->value());
          }
        }
        iter_->Prev();
      } while (iter_->Valid());
    }
    if (value_type == ValueType::kDeletion) {
      Invalidate();
      direction_ = Direction::kForward;
    } else {
      valid_ = true;
    }
  }

  void EnforceUpperBound() {
    if (valid_ && bounds_.upper && user_comparator_->Compare(key(), *bounds_.upper) >= 0) {
      Invalidate();
    }
  }

  void EnforceLowerBound() {
    if (valid_ && bounds_.lower && user_comparator_->Compare(key(), *bounds_.lower) < 0) {
      Invalidate();
    }
  }

  const Comparator* const user_comparator_;
  // Declared before iter_, so what it reads from outlives it.
  const std::vector<std::shared_ptr<const void>> keep_alive_;
  const std::unique_ptr<Iterator> iter_;
  const SequenceNumber sequence_;
  const DBIterBounds bounds_;
  Status status_;
  std::string saved_key_;
  std::string saved_value_;
  Direction direction_ = Direction::kForward;
  bool valid_ = false;
};

}  // namespace

std::unique_ptr<Iterator> NewDBIterator(const Comparator* user_comparator,
                                        std::unique_ptr<Iterator> internal, SequenceNumber sequence,
                                        DBIterBounds bounds,
                                        std::vector<std::shared_ptr<const void>> keep_alive) {
  return std::make_unique<DBIter>(user_comparator, std::move(internal), sequence, std::move(bounds),
                                  std::move(keep_alive));
}

namespace {

class ErrorIterator final : public Iterator {
 public:
  explicit ErrorIterator(Status status) : status_(std::move(status)) {}
  [[nodiscard]] bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Seek(Slice /*target*/) override {}
  void Next() override {}
  void Prev() override {}
  [[nodiscard]] Slice key() const override { return {}; }
  [[nodiscard]] Slice value() const override { return {}; }
  [[nodiscard]] Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewErrorIterator(Status status) {
  return std::make_unique<ErrorIterator>(std::move(status));
}

}  // namespace strata

#include "db/db_iter.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "db/dbformat.hpp"
#include "util/comparator.hpp"

namespace strata {
namespace {

// Internal-key entries, kept sorted the way every engine iterator yields them.
class InternalVectorIterator final : public Iterator {
 public:
  explicit InternalVectorIterator(std::vector<std::pair<std::string, std::string>> entries)
      : entries_(std::move(entries)) {}

  [[nodiscard]] bool Valid() const override { return index_ < entries_.size(); }
  void SeekToFirst() override { index_ = 0; }
  void SeekToLast() override { index_ = entries_.empty() ? 0 : entries_.size() - 1; }
  void Seek(Slice target) override {
    index_ = 0;
    while (index_ < entries_.size() && Compare(entries_[index_].first, target) < 0) {
      ++index_;
    }
  }
  void Next() override { ++index_; }
  void Prev() override { index_ = index_ == 0 ? entries_.size() : index_ - 1; }
  [[nodiscard]] Slice key() const override { return entries_[index_].first; }
  [[nodiscard]] Slice value() const override { return entries_[index_].second; }
  [[nodiscard]] Status status() const override { return Status::Ok(); }

  static int Compare(Slice a, Slice b) { return Comparator().Compare(a, b); }
  static const InternalKeyComparator& Comparator() {
    static const InternalKeyComparator comparator(BytewiseComparator());
    return comparator;
  }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
  std::size_t index_ = 0;
};

struct Write {
  std::string key;
  std::optional<std::string> value;  // nullopt: a delete
};

// Builds the internal stream for `writes`, applied with sequence numbers 1, 2,
// ... in order.
std::unique_ptr<Iterator> Stream(const std::vector<Write>& writes) {
  std::vector<std::pair<std::string, std::string>> entries;
  SequenceNumber sequence = 0;
  for (const Write& write : writes) {
    std::string internal;
    AppendInternalKey(&internal,
                      ParsedInternalKey{write.key, ++sequence,
                                        write.value ? ValueType::kValue : ValueType::kDeletion});
    entries.emplace_back(std::move(internal), write.value.value_or(""));
  }
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    return InternalVectorIterator::Compare(a.first, b.first) < 0;
  });
  return std::make_unique<InternalVectorIterator>(std::move(entries));
}

// What a reader at `sequence` must see.
std::map<std::string, std::string> Model(const std::vector<Write>& writes,
                                         SequenceNumber sequence) {
  std::map<std::string, std::string> model;
  for (std::size_t index = 0; index < writes.size() && index < sequence; ++index) {
    if (writes[index].value) {
      model[writes[index].key] = *writes[index].value;
    } else {
      model.erase(writes[index].key);
    }
  }
  return model;
}

std::unique_ptr<Iterator> NewIter(const std::vector<Write>& writes, SequenceNumber sequence,
                                  DBIterBounds bounds = {}) {
  return NewDBIterator(BytewiseComparator(), Stream(writes), sequence, std::move(bounds), {});
}

std::vector<std::pair<std::string, std::string>> Forward(Iterator* it) {
  std::vector<std::pair<std::string, std::string>> out;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out.emplace_back(ToString(it->key()), ToString(it->value()));
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> Backward(Iterator* it) {
  std::vector<std::pair<std::string, std::string>> out;
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    out.emplace_back(ToString(it->key()), ToString(it->value()));
  }
  return out;
}

using Entries = std::vector<std::pair<std::string, std::string>>;

TEST(DBIterTest, ShowsTheNewestVersionOfEachKey) {
  const std::vector<Write> writes = {{"a", "1"}, {"b", "1"}, {"a", "2"}, {"c", "1"}};
  auto it = NewIter(writes, kMaxSequenceNumber);
  EXPECT_EQ(Forward(it.get()), (Entries{{"a", "2"}, {"b", "1"}, {"c", "1"}}));
  EXPECT_EQ(Backward(it.get()), (Entries{{"c", "1"}, {"b", "1"}, {"a", "2"}}));
}

TEST(DBIterTest, HidesDeletedKeys) {
  const std::vector<Write> writes = {{"a", "1"}, {"b", "1"},          {"b", std::nullopt},
                                     {"c", "1"}, {"c", std::nullopt}, {"c", "3"}};
  auto it = NewIter(writes, kMaxSequenceNumber);
  EXPECT_EQ(Forward(it.get()), (Entries{{"a", "1"}, {"c", "3"}}));
  EXPECT_EQ(Backward(it.get()), (Entries{{"c", "3"}, {"a", "1"}}));
}

TEST(DBIterTest, ReadsAsOfItsSequenceNumber) {
  const std::vector<Write> writes = {{"a", "1"}, {"a", "2"}, {"b", "1"}, {"a", std::nullopt}};
  auto at_two = NewIter(writes, 2);
  EXPECT_EQ(Forward(at_two.get()), (Entries{{"a", "2"}}));
  auto at_three = NewIter(writes, 3);
  EXPECT_EQ(Forward(at_three.get()), (Entries{{"a", "2"}, {"b", "1"}}));
  auto latest = NewIter(writes, 4);
  EXPECT_EQ(Forward(latest.get()), (Entries{{"b", "1"}}));
}

TEST(DBIterTest, SeekLandsOnTheFirstVisibleKeyAtOrAfterTheTarget) {
  const std::vector<Write> writes = {{"a", "1"}, {"c", "1"}, {"e", "1"}, {"c", std::nullopt}};
  auto it = NewIter(writes, kMaxSequenceNumber);
  it->Seek("b");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ToString(it->key()), "e") << "c is deleted";
  it->Seek("f");
  EXPECT_FALSE(it->Valid());
}

TEST(DBIterTest, EmptyAndAllDeletedStreamsAreEmpty) {
  auto empty = NewIter({}, kMaxSequenceNumber);
  EXPECT_TRUE(Forward(empty.get()).empty());
  EXPECT_TRUE(Backward(empty.get()).empty());
  auto deleted = NewIter({{"a", "1"}, {"a", std::nullopt}}, kMaxSequenceNumber);
  EXPECT_TRUE(Forward(deleted.get()).empty());
  EXPECT_TRUE(Backward(deleted.get()).empty());
}

TEST(DBIterTest, BoundsLimitTheScanInBothDirections) {
  const std::vector<Write> writes = {{"a", "1"}, {"b", "1"}, {"c", "1"}, {"d", "1"}, {"e", "1"}};
  auto it = NewIter(writes, kMaxSequenceNumber, {.lower = "b", .upper = "d"});
  EXPECT_EQ(Forward(it.get()), (Entries{{"b", "1"}, {"c", "1"}}));
  EXPECT_EQ(Backward(it.get()), (Entries{{"c", "1"}, {"b", "1"}}));
  it->Seek("a");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ToString(it->key()), "b") << "a seek below the lower bound starts at it";
  it->Seek("d");
  EXPECT_FALSE(it->Valid()) << "the upper bound is exclusive";
}

TEST(DBIterTest, AMalformedInternalKeyStopsTheScanWithCorruption) {
  std::string good;
  AppendInternalKey(&good, ParsedInternalKey{"a", 1, ValueType::kValue});
  auto stream = std::make_unique<InternalVectorIterator>(
      std::vector<std::pair<std::string, std::string>>{{good, "1"}, {"bad", "x"}});
  auto it = NewDBIterator(BytewiseComparator(), std::move(stream), kMaxSequenceNumber, {}, {});
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  it->Next();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

// Random histories, random read points, random walks mixing Next, Prev, and
// Seek, all checked against a std::map built for the read's sequence number.
TEST(DBIterTest, RandomWalksMatchTheModel) {
  std::mt19937 random(2026);
  for (int trial = 0; trial < 300; ++trial) {
    std::vector<Write> writes;
    const int count = 1 + static_cast<int>(random() % 60);
    for (int index = 0; index < count; ++index) {
      const std::string key = "k" + std::to_string(random() % 12);
      if (random() % 4 == 0) {
        writes.push_back({key, std::nullopt});
      } else {
        writes.push_back({key, "v" + std::to_string(index)});
      }
    }
    const SequenceNumber sequence = random() % (writes.size() + 1);
    std::optional<std::string> lower;
    std::optional<std::string> upper;
    if (random() % 3 == 0) {
      lower = "k" + std::to_string(random() % 12);
    }
    if (random() % 3 == 0) {
      upper = "k" + std::to_string(random() % 12);
    }
    std::map<std::string, std::string> model = Model(writes, sequence);
    std::erase_if(model, [&](const auto& entry) {
      return (lower && entry.first < *lower) || (upper && entry.first >= *upper);
    });

    auto it = NewIter(writes, sequence, {.lower = lower, .upper = upper});
    Entries expected(model.begin(), model.end());
    ASSERT_EQ(Forward(it.get()), expected) << "trial " << trial;
    std::reverse(expected.begin(), expected.end());
    ASSERT_EQ(Backward(it.get()), expected) << "trial " << trial;

    // A random walk: the model position is a std::map iterator.
    auto position = model.end();
    bool valid = false;
    for (int step = 0; step < 40; ++step) {
      const auto action = random() % 5;
      if (action == 0 || !valid) {
        const std::string target = "k" + std::to_string(random() % 13);
        it->Seek(target);
        position = model.lower_bound(std::max(target, lower.value_or("")));
      } else if (action == 1) {
        it->SeekToLast();
        position = model.empty() ? model.end() : std::prev(model.end());
      } else if (action <= 3) {
        it->Next();
        ++position;
      } else {
        it->Prev();
        position = position == model.begin() ? model.end() : std::prev(position);
      }
      valid = position != model.end();
      ASSERT_EQ(it->Valid(), valid) << "trial " << trial << " step " << step;
      if (valid) {
        ASSERT_EQ(ToString(it->key()), position->first) << "trial " << trial << " step " << step;
        ASSERT_EQ(ToString(it->value()), position->second);
      }
    }
  }
}

}  // namespace
}  // namespace strata

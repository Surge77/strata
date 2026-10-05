#include "table/merger.hpp"

#include "strata/iterator.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "util/comparator.hpp"

namespace {

// A sorted in-memory child that can be told to fail at a given key.
class VectorIterator final : public strata::Iterator {
 public:
  explicit VectorIterator(std::vector<std::pair<std::string, std::string>> entries,
                          std::string fail_at = "")
      : entries_(std::move(entries)), fail_at_(std::move(fail_at)) {}

  [[nodiscard]] bool Valid() const override { return index_ < entries_.size() && !failed_; }
  void SeekToFirst() override { Move(0); }
  void SeekToLast() override { Move(entries_.empty() ? 0 : entries_.size() - 1); }
  void Seek(strata::Slice target) override {
    std::size_t index = 0;
    while (index < entries_.size() && strata::Slice(entries_[index].first) < target) {
      ++index;
    }
    Move(index);
  }
  void Next() override { Move(index_ + 1); }
  void Prev() override { Move(index_ == 0 ? entries_.size() : index_ - 1); }
  [[nodiscard]] strata::Slice key() const override { return entries_[index_].first; }
  [[nodiscard]] strata::Slice value() const override { return entries_[index_].second; }
  [[nodiscard]] strata::Status status() const override {
    return failed_ ? strata::Status::Corruption("injected child failure") : strata::Status::Ok();
  }

 private:
  void Move(std::size_t index) {
    index_ = index;
    if (!fail_at_.empty() && index_ < entries_.size() && entries_[index_].first == fail_at_) {
      failed_ = true;
    }
  }

  std::vector<std::pair<std::string, std::string>> entries_;
  std::string fail_at_;
  std::size_t index_ = 0;
  bool failed_ = false;
};

using Entries = std::vector<std::pair<std::string, std::string>>;

std::unique_ptr<strata::Iterator> Merge(const std::vector<Entries>& children,
                                        const std::string& fail_at = "") {
  std::vector<std::unique_ptr<strata::Iterator>> iterators;
  for (std::size_t index = 0; index < children.size(); ++index) {
    iterators.push_back(
        std::make_unique<VectorIterator>(children[index], index == 0 ? fail_at : ""));
  }
  return strata::table::NewMergingIterator(strata::BytewiseComparator(), std::move(iterators));
}

std::vector<std::string> ForwardKeys(strata::Iterator* it) {
  std::vector<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    keys.push_back(strata::ToString(it->key()));
  }
  return keys;
}

std::vector<std::string> BackwardKeys(strata::Iterator* it) {
  std::vector<std::string> keys;
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    keys.push_back(strata::ToString(it->key()));
  }
  return keys;
}

TEST(MergerTest, NoChildrenIsEmpty) {
  auto it = Merge({});
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

TEST(MergerTest, MergesChildrenInKeyOrder) {
  auto it = Merge({{{"a", "1"}, {"d", "4"}}, {{"b", "2"}, {"e", "5"}}, {}, {{"c", "3"}}});
  EXPECT_EQ(ForwardKeys(it.get()), (std::vector<std::string>{"a", "b", "c", "d", "e"}));
  EXPECT_EQ(BackwardKeys(it.get()), (std::vector<std::string>{"e", "d", "c", "b", "a"}));
}

TEST(MergerTest, ValuesTravelWithTheirKeys) {
  auto it = Merge({{{"a", "first"}}, {{"b", "second"}}});
  it->Seek("b");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(strata::ToString(it->value()), "second");
}

TEST(MergerTest, SeekFindsTheFirstKeyAtOrAfterTheTarget) {
  auto it = Merge({{{"a", ""}, {"m", ""}}, {{"f", ""}, {"z", ""}}});
  it->Seek("g");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(strata::ToString(it->key()), "m");
  it->Seek("zz");
  EXPECT_FALSE(it->Valid());
}

TEST(MergerTest, ChangingDirectionVisitsNeighbours) {
  auto it = Merge({{{"a", ""}, {"c", ""}, {"e", ""}}, {{"b", ""}, {"d", ""}}});
  it->Seek("c");
  it->Prev();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(strata::ToString(it->key()), "b");
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(strata::ToString(it->key()), "c");
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(strata::ToString(it->key()), "d");
  it->Prev();
  it->Prev();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(strata::ToString(it->key()), "b");
}

TEST(MergerTest, AChildErrorStopsTheMergeInsteadOfSkippingTheChild) {
  auto it = Merge({{{"a", ""}, {"c", ""}, {"e", ""}}, {{"b", ""}, {"d", ""}, {"f", ""}}}, "c");
  std::vector<std::string> seen;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    seen.push_back(strata::ToString(it->key()));
  }
  // The failing child breaks as soon as it advances past "a", before "b" is
  // emitted: the merge stops at the first sign of trouble.
  EXPECT_EQ(seen, (std::vector<std::string>{"a"})) << "must not carry on with b, d, f";
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST(MergerTest, RandomMergesMatchASortedReference) {
  std::mt19937 random(7);
  for (int trial = 0; trial < 50; ++trial) {
    std::map<std::string, std::string> reference;
    std::vector<Entries> children(1 + random() % 5);
    for (int entry = 0; entry < 60; ++entry) {
      const std::string key = "k" + std::to_string(random() % 1000);
      if (reference.contains(key)) {
        continue;  // children hold distinct keys, as internal keys always are
      }
      reference[key] = "v" + key;
      children[random() % children.size()].emplace_back(key, "v" + key);
    }
    for (auto& child : children) {
      std::sort(child.begin(), child.end());
    }
    auto it = Merge(children);
    std::vector<std::string> expected;
    for (const auto& [key, value] : reference) {
      expected.push_back(key);
    }
    EXPECT_EQ(ForwardKeys(it.get()), expected);
    std::reverse(expected.begin(), expected.end());
    EXPECT_EQ(BackwardKeys(it.get()), expected);
  }
}

}  // namespace

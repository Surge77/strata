#include "db/memtable.hpp"

#include <map>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "db/dbformat.hpp"

namespace {

using strata::LookupKey;
using strata::MemTable;
using strata::ParsedInternalKey;
using strata::SequenceNumber;
using strata::ValueType;
using Result = MemTable::LookupResult;

const strata::InternalKeyComparator* Comparator() {
  static const strata::InternalKeyComparator kComparator(strata::BytewiseComparator());
  return &kComparator;
}

Result Lookup(const MemTable& table, std::string_view key, SequenceNumber sequence,
              std::string* value) {
  return table.Get(LookupKey(key, sequence), value);
}

TEST(MemTable, EmptyTableHasNothing) {
  const MemTable table{Comparator()};
  std::string value;
  EXPECT_EQ(Lookup(table, "k", 100, &value), Result::kNotPresent);
}

TEST(MemTable, ReturnsTheNewestVersion) {
  MemTable table{Comparator()};
  table.Add(1, ValueType::kValue, "k", "v1");
  table.Add(2, ValueType::kValue, "k", "v2");
  table.Add(3, ValueType::kValue, "other", "x");
  std::string value;
  ASSERT_EQ(Lookup(table, "k", 10, &value), Result::kFound);
  EXPECT_EQ(value, "v2");
}

// Reading at an older sequence number sees the state as of that point: the
// mechanism snapshots are built on.
TEST(MemTable, ReadsAtAnOlderSequenceSeeTheOlderVersion) {
  MemTable table{Comparator()};
  table.Add(5, ValueType::kValue, "k", "at-5");
  table.Add(9, ValueType::kValue, "k", "at-9");
  std::string value;
  ASSERT_EQ(Lookup(table, "k", 8, &value), Result::kFound);
  EXPECT_EQ(value, "at-5");
  ASSERT_EQ(Lookup(table, "k", 9, &value), Result::kFound);
  EXPECT_EQ(value, "at-9");
  EXPECT_EQ(Lookup(table, "k", 4, &value), Result::kNotPresent);
}

// A tombstone is not "absent": it must stop the search so older files cannot
// resurrect the key.
TEST(MemTable, DeletionIsReportedDistinctlyFromAbsence) {
  MemTable table{Comparator()};
  table.Add(1, ValueType::kValue, "k", "v");
  table.Add(2, ValueType::kDeletion, "k", "");
  std::string value;
  EXPECT_EQ(Lookup(table, "k", 2, &value), Result::kDeleted);
  EXPECT_EQ(Lookup(table, "k", 1, &value), Result::kFound);
  table.Add(3, ValueType::kValue, "k", "back");
  ASSERT_EQ(Lookup(table, "k", 3, &value), Result::kFound);
  EXPECT_EQ(value, "back");
}

// A key that is a prefix of another must not match it: the user key
// comparison, not just the seek position, decides.
TEST(MemTable, PrefixKeysAreDistinct) {
  MemTable table{Comparator()};
  table.Add(1, ValueType::kValue, "abc", "long");
  std::string value;
  EXPECT_EQ(Lookup(table, "ab", 5, &value), Result::kNotPresent);
  EXPECT_EQ(Lookup(table, "abcd", 5, &value), Result::kNotPresent);
}

// A default-constructed Slice has a null data(); the DB passes one as the value
// of every Delete. Copying it must not hand memcpy a null pointer (UBSan).
TEST(MemTable, AcceptsNullEmptySlices) {
  MemTable table{Comparator()};
  table.Add(1, ValueType::kDeletion, strata::Slice(), strata::Slice());
  table.Add(2, ValueType::kValue, "k", strata::Slice());
  std::string value = "unchanged";
  EXPECT_EQ(Lookup(table, "", 5, &value), Result::kDeleted);
  ASSERT_EQ(Lookup(table, "k", 5, &value), Result::kFound);
  EXPECT_EQ(value, "");
}

TEST(MemTable, KeysAndValuesAreBinarySafe) {
  MemTable table{Comparator()};
  const std::string key("a\0b", 3);
  const std::string payload(10000, '\0');
  table.Add(1, ValueType::kValue, key, payload);
  table.Add(2, ValueType::kValue, "", "empty-key");
  std::string value;
  ASSERT_EQ(Lookup(table, key, 1, &value), Result::kFound);
  EXPECT_EQ(value, payload);
  ASSERT_EQ(Lookup(table, "", 2, &value), Result::kFound);
  EXPECT_EQ(value, "empty-key");
}

TEST(MemTable, IteratesInInternalKeyOrder) {
  MemTable table{Comparator()};
  table.Add(3, ValueType::kValue, "b", "b3");
  table.Add(1, ValueType::kValue, "a", "a1");
  table.Add(4, ValueType::kDeletion, "a", "");
  table.Add(2, ValueType::kValue, "c", "c2");

  auto it = table.NewIterator();
  std::vector<std::string> rendered;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ParsedInternalKey parsed;
    ASSERT_TRUE(strata::ParseInternalKey(it->key(), &parsed));
    rendered.push_back(
        std::string(parsed.user_key) + "@" + std::to_string(parsed.sequence) +
        (parsed.type == ValueType::kDeletion ? "-del" : "=" + std::string(it->value())));
  }
  // User key ascending; newest first within a key.
  EXPECT_EQ(rendered, (std::vector<std::string>{"a@4-del", "a@1=a1", "b@3=b3", "c@2=c2"}));
  EXPECT_TRUE(it->status().ok());

  it->SeekToLast();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value(), "c2");
  it->Prev();
  EXPECT_EQ(it->value(), "b3");

  std::string target;
  strata::AppendInternalKey(&target, {"b", strata::kMaxSequenceNumber, strata::kValueTypeForSeek});
  it->Seek(target);
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value(), "b3");
}

TEST(MemTable, MemoryUsageGrowsWithData) {
  MemTable table{Comparator()};
  const std::size_t empty = table.ApproximateMemoryUsage();
  for (SequenceNumber sequence = 1; sequence <= 1000; ++sequence) {
    table.Add(sequence, ValueType::kValue, "key" + std::to_string(sequence), std::string(100, 'v'));
  }
  EXPECT_GT(table.ApproximateMemoryUsage(), empty + 100U * 1000U);
}

// Differential test against a model that stores every version.
TEST(MemTable, AgreesWithAVersionedModel) {
  std::mt19937 random(11);
  MemTable table{Comparator()};
  std::map<std::string, std::map<SequenceNumber, std::string>> model;  // "" = tombstone
  for (SequenceNumber sequence = 1; sequence <= 3000; ++sequence) {
    const std::string key = "k" + std::to_string(random() % 200);
    if (random() % 4 == 0) {
      table.Add(sequence, ValueType::kDeletion, key, "");
      model[key][sequence] = "";
    } else {
      const std::string value = "v" + std::to_string(sequence);
      table.Add(sequence, ValueType::kValue, key, value);
      model[key][sequence] = value;
    }
  }
  for (int probe = 0; probe < 2000; ++probe) {
    const std::string key = "k" + std::to_string(random() % 220);
    const SequenceNumber at = 1 + random() % 3000;
    Result expected = Result::kNotPresent;
    std::string expected_value;
    if (const auto versions = model.find(key); versions != model.end()) {
      auto newest = versions->second.upper_bound(at);
      if (newest != versions->second.begin()) {
        --newest;
        expected = newest->second.empty() ? Result::kDeleted : Result::kFound;
        expected_value = newest->second;
      }
    }
    std::string value;
    ASSERT_EQ(Lookup(table, key, at, &value), expected) << key << "@" << at;
    if (expected == Result::kFound) {
      ASSERT_EQ(value, expected_value) << key << "@" << at;
    }
  }
}

}  // namespace

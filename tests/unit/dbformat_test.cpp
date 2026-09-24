#include "db/dbformat.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "util/coding.hpp"

namespace {

using strata::InternalKeyComparator;
using strata::LookupKey;
using strata::ParsedInternalKey;
using strata::SequenceNumber;
using strata::Slice;
using strata::ValueType;

std::string Ikey(std::string_view user_key, SequenceNumber sequence, ValueType type) {
  std::string encoded;
  strata::AppendInternalKey(&encoded, {user_key, sequence, type});
  return encoded;
}

TEST(InternalKey, EncodesUserKeyThenLittleEndianTrailer) {
  const std::string encoded = Ikey("abc", 0x0102, ValueType::kValue);
  ASSERT_EQ(encoded.size(), 3U + 8U);
  EXPECT_EQ(Slice(encoded).substr(0, 3), "abc");
  EXPECT_EQ(strata::DecodeFixed64(encoded.data() + 3), (0x0102ULL << 8U) | 1U);
}

TEST(InternalKey, ParsesWhatItEncodes) {
  for (const SequenceNumber sequence : {SequenceNumber{0}, SequenceNumber{1},
                                        SequenceNumber{1} << 40, strata::kMaxSequenceNumber}) {
    for (const ValueType type : {ValueType::kValue, ValueType::kDeletion}) {
      const std::string encoded = Ikey("key", sequence, type);
      ParsedInternalKey parsed;
      ASSERT_TRUE(strata::ParseInternalKey(encoded, &parsed));
      EXPECT_EQ(parsed.user_key, "key");
      EXPECT_EQ(parsed.sequence, sequence);
      EXPECT_EQ(parsed.type, type);
      EXPECT_EQ(strata::ExtractUserKey(encoded), "key");
    }
  }
}

TEST(InternalKey, RejectsShortKeysAndUnknownTypes) {
  ParsedInternalKey parsed;
  EXPECT_FALSE(strata::ParseInternalKey("1234567", &parsed));
  std::string bad_type = Ikey("k", 5, ValueType::kValue);
  bad_type[1] = 0x02;  // low byte of the trailer is the type
  EXPECT_FALSE(strata::ParseInternalKey(bad_type, &parsed));
}

// The whole point of the format: user key ascending, newest version first.
TEST(InternalKey, SortsByUserKeyThenNewestFirst) {
  std::vector<std::string> keys = {
      Ikey("b", 1, ValueType::kValue),    Ikey("a", 1, ValueType::kValue),
      Ikey("a", 9, ValueType::kDeletion), Ikey("a", 9, ValueType::kValue),
      Ikey("", 3, ValueType::kValue),     Ikey("aa", 2, ValueType::kValue),
      Ikey("a", 5, ValueType::kValue),
  };
  const InternalKeyComparator comparator;
  std::sort(keys.begin(), keys.end(), [&](const std::string& x, const std::string& y) {
    return comparator.Compare(x, y) < 0;
  });
  EXPECT_EQ(keys, (std::vector<std::string>{
                      Ikey("", 3, ValueType::kValue),
                      Ikey("a", 9, ValueType::kValue),
                      Ikey("a", 9, ValueType::kDeletion),
                      Ikey("a", 5, ValueType::kValue),
                      Ikey("a", 1, ValueType::kValue),
                      Ikey("aa", 2, ValueType::kValue),
                      Ikey("b", 1, ValueType::kValue),
                  }));
  EXPECT_EQ(comparator.Compare(keys[1], keys[1]), 0);
}

// User keys compare as unsigned bytes: 0xFF sorts after 'a', never before.
TEST(InternalKey, ComparesUserKeysAsUnsignedBytes) {
  const InternalKeyComparator comparator;
  EXPECT_LT(comparator.Compare(Ikey("a", 1, ValueType::kValue), Ikey("\xff", 1, ValueType::kValue)),
            0);
}

// A seek key at sequence s must land on a write made at exactly s -- both a
// value and a tombstone -- rather than skipping past it.
TEST(LookupKey, SortsBeforeEveryEntryAtItsOwnSequence) {
  const LookupKey lookup("k", 7);
  const InternalKeyComparator comparator;
  EXPECT_LE(comparator.Compare(lookup.internal_key(), Ikey("k", 7, ValueType::kValue)), 0);
  EXPECT_LT(comparator.Compare(lookup.internal_key(), Ikey("k", 7, ValueType::kDeletion)), 0);
  EXPECT_GT(comparator.Compare(lookup.internal_key(), Ikey("k", 8, ValueType::kValue)), 0);
  EXPECT_LT(comparator.Compare(lookup.internal_key(), Ikey("k", 6, ValueType::kValue)), 0);
}

TEST(LookupKey, ExposesAllThreeRepresentations) {
  const LookupKey lookup("user", 42);
  EXPECT_EQ(lookup.user_key(), "user");
  EXPECT_EQ(lookup.internal_key(), Ikey("user", 42, ValueType::kValue));
  Slice memtable_key = lookup.memtable_key();
  std::uint32_t length = 0;
  ASSERT_TRUE(strata::GetVarint32(&memtable_key, &length));
  EXPECT_EQ(length, 4U + 8U);
  EXPECT_EQ(memtable_key, lookup.internal_key());
}

}  // namespace

#include "util/comparator.hpp"

#include <random>
#include <string>

#include <gtest/gtest.h>

#include "db/dbformat.hpp"

namespace {

using strata::BytewiseComparator;
using strata::InternalKeyComparator;
using strata::ValueType;

TEST(BytewiseComparator, OrdersAsUnsignedBytes) {
  const auto* comparator = BytewiseComparator();
  EXPECT_LT(comparator->Compare("a", "b"), 0);
  EXPECT_LT(comparator->Compare("a", "ab"), 0);
  EXPECT_LT(comparator->Compare("a", "\xff"), 0);
  EXPECT_EQ(comparator->Compare("same", "same"), 0);
  EXPECT_EQ(comparator->Name(), "strata.BytewiseComparator");
}

TEST(BytewiseComparator, ShortensSeparators) {
  const auto* comparator = BytewiseComparator();
  std::string start = "abcxyz";
  comparator->FindShortestSeparator(&start, "abezz");
  EXPECT_EQ(start, "abd");

  std::string prefix = "abc";
  comparator->FindShortestSeparator(&prefix, "abcdef");  // start is a prefix of limit
  EXPECT_EQ(prefix, "abc");

  std::string adjacent = "abc1";
  comparator->FindShortestSeparator(&adjacent, "abc2");  // no room between the bytes
  EXPECT_EQ(adjacent, "abc1");

  std::string maxed = std::string("a\xff") + "zz";
  comparator->FindShortestSeparator(&maxed, "b");
  EXPECT_EQ(maxed, std::string("a\xff") + "zz") << "0xFF cannot be bumped";
}

TEST(BytewiseComparator, ShortensSuccessors) {
  const auto* comparator = BytewiseComparator();
  std::string key = "abc";
  comparator->FindShortSuccessor(&key);
  EXPECT_EQ(key, "b");
  std::string leading_ff = std::string("\xff\xff") + "x";
  comparator->FindShortSuccessor(&leading_ff);
  EXPECT_EQ(leading_ff, std::string("\xff\xff") + "y");
  std::string all_ff = "\xff\xff";
  comparator->FindShortSuccessor(&all_ff);
  EXPECT_EQ(all_ff, "\xff\xff") << "no shorter successor exists";
}

// The property index blocks rely on: for any start < limit, the separator is
// in [start, limit), and a successor is >= its input. Checked on random keys.
TEST(BytewiseComparator, SeparatorAndSuccessorKeepTheirInvariants) {
  const auto* comparator = BytewiseComparator();
  std::mt19937 random(5);
  auto random_key = [&random] {
    std::string key(random() % 8, '\0');
    for (char& byte : key) {
      byte = static_cast<char>(random() % 4 == 0 ? 0xFF : random() % 256);
    }
    return key;
  };
  for (int trial = 0; trial < 20000; ++trial) {
    std::string a = random_key();
    std::string b = random_key();
    if (comparator->Compare(a, b) > 0) {
      std::swap(a, b);
    }
    if (comparator->Compare(a, b) == 0) {
      continue;
    }
    std::string separator = a;
    comparator->FindShortestSeparator(&separator, b);
    ASSERT_LE(comparator->Compare(a, separator), 0);
    ASSERT_LT(comparator->Compare(separator, b), 0);
    std::string successor = a;
    comparator->FindShortSuccessor(&successor);
    ASSERT_GE(comparator->Compare(successor, a), 0);
  }
}

std::string Ikey(std::string_view user_key, strata::SequenceNumber sequence) {
  std::string encoded;
  strata::AppendInternalKey(&encoded, {user_key, sequence, ValueType::kValue});
  return encoded;
}

// Shortened internal separators must still sort strictly between the last key
// of one block and the first key of the next -- including every older version
// of that next key.
TEST(InternalKeyComparator, SeparatorsStayBetweenTheirBounds) {
  const InternalKeyComparator comparator(BytewiseComparator());
  EXPECT_EQ(comparator.Name(), "strata.InternalKeyComparator");
  EXPECT_EQ(comparator.user_comparator(), BytewiseComparator());

  std::string start = Ikey("foo", 100);
  comparator.FindShortestSeparator(&start, Ikey("hello", 200));
  EXPECT_EQ(start, Ikey("g", strata::kMaxSequenceNumber));
  EXPECT_LT(comparator.Compare(Ikey("foo", 100), start), 0);
  EXPECT_LT(comparator.Compare(start, Ikey("hello", 200)), 0);

  std::string same_user_key = Ikey("foo", 100);
  comparator.FindShortestSeparator(&same_user_key, Ikey("foo", 99));
  EXPECT_EQ(same_user_key, Ikey("foo", 100)) << "same user key: nothing to shorten";

  std::string successor = Ikey("foo", 100);
  comparator.FindShortSuccessor(&successor);
  EXPECT_EQ(successor, Ikey("g", strata::kMaxSequenceNumber));
  EXPECT_LT(comparator.Compare(Ikey("foo", 100), successor), 0);

  std::string unshortenable = Ikey("\xff\xff", 1);
  comparator.FindShortSuccessor(&unshortenable);
  EXPECT_EQ(unshortenable, Ikey("\xff\xff", 1));
}

}  // namespace

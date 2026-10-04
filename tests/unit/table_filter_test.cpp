#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "db/dbformat.hpp"
#include "support/string_file.hpp"
#include "table/bloom_filter.hpp"
#include "table/format.hpp"
#include "table/table.hpp"
#include "table/table_builder.hpp"
#include "util/coding.hpp"
#include "util/crc32c.hpp"

namespace {

class CountingFile final : public strata::RandomAccessFile {
 public:
  CountingFile(std::string contents, int* reads) : contents_(std::move(contents)), reads_(reads) {}

  strata::Result<strata::Slice> Read(std::uint64_t offset, std::size_t n,
                                     char* scratch) const override {
    ++*reads_;
    if (offset >= contents_.size()) {
      return strata::Slice(scratch, 0);
    }
    const std::size_t count = std::min<std::size_t>(n, contents_.size() - offset);
    std::memcpy(scratch, contents_.data() + offset, count);
    return strata::Slice(scratch, count);
  }

 private:
  std::string contents_;
  int* reads_;
};

class CaseInsensitiveComparator final : public strata::Comparator {
 public:
  int Compare(strata::Slice a, strata::Slice b) const override {
    const std::size_t common = std::min(a.size(), b.size());
    for (std::size_t index = 0; index < common; ++index) {
      const int left = Fold(a[index]);
      const int right = Fold(b[index]);
      if (left != right) {
        return left - right;
      }
    }
    return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
  }
  std::string_view Name() const override { return "test.CaseInsensitive"; }
  void FindShortestSeparator(std::string* /*start*/, strata::Slice /*limit*/) const override {}
  void FindShortSuccessor(std::string* /*key*/) const override {}

 private:
  static int Fold(char byte) { return byte >= 'A' && byte <= 'Z' ? byte - 'A' + 'a' : byte; }
};

std::string InternalKey(const std::string& user_key) {
  std::string encoded;
  strata::AppendInternalKey(&encoded, {user_key, 1, strata::ValueType::kValue});
  return encoded;
}

std::string UserKey(int index) {
  std::string number = std::to_string(index);
  return "key" + std::string(3 - number.size(), '0') + number;
}

TEST(TableFilterTest, StoredKeysStayVisibleAndMostMissingKeysSkipDataReads) {
  const strata::InternalKeyComparator comparator(strata::BytewiseComparator());
  const strata::table::TableOptions options{
      .comparator = &comparator, .block_size = 128, .use_filter = true};
  std::string contents;
  strata::test::StringSink sink(&contents);
  strata::table::TableBuilder builder(options, &sink);
  for (int index = 0; index < 200; index += 2) {
    ASSERT_TRUE(builder.Add(InternalKey(UserKey(index)), "value").ok());
  }
  ASSERT_TRUE(builder.Finish().ok());

  int reads = 0;
  auto table = strata::table::Table::Open(options, std::make_unique<CountingFile>(contents, &reads),
                                          contents.size());
  ASSERT_TRUE(table.ok()) << table.status().ToString();

  bool found = false;
  const strata::LookupKey present(UserKey(100), strata::kMaxSequenceNumber);
  ASSERT_TRUE((*table)
                  ->Seek(present.internal_key(),
                         [&](strata::Slice key, strata::Slice value) {
                           found = strata::ExtractUserKey(key) == "key100" && value == "value";
                         })
                  .ok());
  EXPECT_TRUE(found);

  int skipped = 0;
  for (int index = 1; index < 199; index += 2) {
    const strata::LookupKey missing(UserKey(index), strata::kMaxSequenceNumber);
    const int before = reads;
    if ((*table)->MayContainUserKey(missing.user_key())) {
      ASSERT_TRUE((*table)->Seek(missing.internal_key(), [](strata::Slice, strata::Slice) {}).ok());
    }
    skipped += reads == before ? 1 : 0;
  }
  EXPECT_GE(skipped, 90);
}

TEST(TableFilterTest, SeekStillReturnsTheNextKeyAfterAFilterMiss) {
  const strata::InternalKeyComparator comparator(strata::BytewiseComparator());
  const strata::table::TableOptions options{.comparator = &comparator, .use_filter = true};
  std::string contents;
  strata::test::StringSink sink(&contents);
  strata::table::TableBuilder builder(options, &sink);
  ASSERT_TRUE(builder.Add(InternalKey("a"), "first").ok());
  ASSERT_TRUE(builder.Add(InternalKey("c"), "third").ok());
  ASSERT_TRUE(builder.Finish().ok());
  auto table = strata::table::Table::Open(
      options, std::make_unique<strata::test::StringRandomAccessFile>(contents), contents.size());
  ASSERT_TRUE(table.ok());
  EXPECT_FALSE((*table)->MayContainUserKey("b"));
  const strata::LookupKey between("b", strata::kMaxSequenceNumber);
  std::string found;
  ASSERT_TRUE(
      (*table)
          ->Seek(between.internal_key(),
                 [&](strata::Slice key, strata::Slice) { found = strata::ExtractUserKey(key); })
          .ok());
  EXPECT_EQ(found, "c");
}

TEST(TableFilterTest, NonBytewiseComparatorNeverUsesABytewiseFilter) {
  const CaseInsensitiveComparator user_comparator;
  const strata::InternalKeyComparator custom(&user_comparator);
  std::string contents;
  strata::test::StringSink sink(&contents);
  strata::table::TableBuilder rejected({.comparator = &custom, .use_filter = true}, &sink);
  EXPECT_TRUE(rejected.Add(InternalKey("Key"), "value").IsInvalidArgument());

  const strata::InternalKeyComparator bytewise(strata::BytewiseComparator());
  const strata::table::TableOptions options{.comparator = &bytewise, .use_filter = true};
  strata::table::TableBuilder builder(options, &sink);
  ASSERT_TRUE(builder.Add(InternalKey("Key"), "value").ok());
  ASSERT_TRUE(builder.Finish().ok());
  auto table = strata::table::Table::Open(
      {.comparator = &custom}, std::make_unique<strata::test::StringRandomAccessFile>(contents),
      contents.size());
  ASSERT_TRUE(table.ok());
  EXPECT_TRUE((*table)->MayContainUserKey("key"));
  const strata::LookupKey lookup("key", strata::kMaxSequenceNumber);
  std::string found;
  ASSERT_TRUE(
      (*table)
          ->Seek(lookup.internal_key(),
                 [&](strata::Slice key, strata::Slice) { found = strata::ExtractUserKey(key); })
          .ok());
  EXPECT_EQ(found, "Key");
}

TEST(TableFilterTest, InvalidFilterWithValidChecksumFailsOpen) {
  const strata::InternalKeyComparator comparator(strata::BytewiseComparator());
  const strata::table::TableOptions options{.comparator = &comparator, .use_filter = true};
  std::string contents;
  strata::test::StringSink sink(&contents);
  strata::table::TableBuilder builder(options, &sink);
  ASSERT_TRUE(builder.Add(InternalKey("key"), "value").ok());
  ASSERT_TRUE(builder.Finish().ok());

  strata::table::Footer footer;
  ASSERT_TRUE(footer
                  .DecodeFrom(strata::Slice(contents).substr(contents.size() -
                                                             strata::table::Footer::kEncodedLength))
                  .ok());
  strata::test::StringRandomAccessFile source(contents);
  auto meta_bytes = strata::table::ReadBlock(source, contents.size(), footer.metaindex_handle);
  ASSERT_TRUE(meta_bytes.ok());
  auto meta = strata::table::Block::Create(std::move(*meta_bytes));
  ASSERT_TRUE(meta.ok());
  auto it = (*meta)->NewIterator(strata::BytewiseComparator());
  it->Seek(strata::table::kUserBloomFilterMetaName);
  ASSERT_TRUE(it->Valid());
  strata::table::BlockHandle handle;
  strata::Slice encoded_handle = it->value();
  ASSERT_TRUE(handle.DecodeFrom(&encoded_handle).ok());

  contents[handle.offset()] = '\x06';
  const strata::Slice filter_and_type(contents.data() + handle.offset(), handle.size() + 1);
  std::string checksum;
  strata::PutFixed32(&checksum, strata::crc32c::Mask(strata::crc32c::Value(filter_and_type)));
  contents.replace(handle.offset() + handle.size() + 1, checksum.size(), checksum);

  auto table = strata::table::Table::Open(
      options, std::make_unique<strata::test::StringRandomAccessFile>(contents), contents.size());
  EXPECT_TRUE(table.status().IsCorruption());
}

}  // namespace

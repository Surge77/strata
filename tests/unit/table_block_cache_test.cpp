#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "support/string_file.hpp"
#include "table/block_cache.hpp"
#include "table/table.hpp"
#include "table/table_builder.hpp"
#include "util/comparator.hpp"

namespace {

using strata::table::BlockCache;
using strata::table::Table;

constexpr std::size_t kCacheBytes = 1024 * 1024;

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

// A one-block table whose every key maps to `value`.
std::string BuildTable(const std::string& value) {
  std::string contents;
  strata::test::StringSink sink(&contents);
  strata::table::TableBuilder builder({.comparator = strata::BytewiseComparator()}, &sink);
  for (const char* key : {"apple", "banana", "cherry"}) {
    EXPECT_TRUE(builder.Add(key, value).ok());
  }
  EXPECT_TRUE(builder.Finish().ok());
  return contents;
}

std::unique_ptr<Table> OpenTable(const std::string& contents, BlockCache* cache,
                                 std::uint64_t cache_id, int* reads) {
  auto opened = Table::Open(
      {.comparator = strata::BytewiseComparator(), .block_cache = cache, .cache_id = cache_id},
      std::make_unique<CountingFile>(contents, reads), contents.size());
  EXPECT_TRUE(opened.ok()) << opened.status().ToString();
  return opened.ok() ? std::move(*opened) : nullptr;
}

std::string ValueAt(const Table& table, const std::string& key) {
  std::string value = "<missing>";
  const strata::Status status =
      table.Seek(key, [&](strata::Slice found_key, strata::Slice found_value) {
        if (found_key == strata::Slice(key)) {
          value.assign(found_value);
        }
      });
  return status.ok() ? value : status.ToString();
}

TEST(TableBlockCacheTest, RepeatedSeeksReadTheDataBlockOnce) {
  BlockCache cache(kCacheBytes);
  int reads = 0;
  const auto table = OpenTable(BuildTable("v"), &cache, 1, &reads);
  ASSERT_NE(table, nullptr);
  const int reads_after_open = reads;
  EXPECT_EQ(ValueAt(*table, "banana"), "v");
  EXPECT_EQ(reads, reads_after_open + 1);
  EXPECT_EQ(ValueAt(*table, "cherry"), "v");
  EXPECT_EQ(ValueAt(*table, "apple"), "v");
  EXPECT_EQ(reads, reads_after_open + 1) << "later seeks must hit the cache";
  EXPECT_GT(cache.usage(), 0U);
}

TEST(TableBlockCacheTest, WithoutACacheEverySeekReadsTheFile) {
  int reads = 0;
  const auto table = OpenTable(BuildTable("v"), nullptr, 0, &reads);
  ASSERT_NE(table, nullptr);
  const int reads_after_open = reads;
  EXPECT_EQ(ValueAt(*table, "banana"), "v");
  EXPECT_EQ(ValueAt(*table, "banana"), "v");
  EXPECT_EQ(reads, reads_after_open + 2);
}

TEST(TableBlockCacheTest, TablesWithDifferentIdsDoNotShareBlocks) {
  BlockCache cache(kCacheBytes);
  int reads = 0;
  // Same layout, so their data blocks start at the same offset.
  const auto first = OpenTable(BuildTable("first"), &cache, 1, &reads);
  const auto second = OpenTable(BuildTable("second"), &cache, 2, &reads);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(ValueAt(*first, "apple"), "first");
  EXPECT_EQ(ValueAt(*second, "apple"), "second");
  EXPECT_EQ(ValueAt(*first, "apple"), "first");
}

TEST(TableBlockCacheTest, IteratorKeepsAnEvictedBlockAlive) {
  BlockCache cache(kCacheBytes);
  int reads = 0;
  const auto table = OpenTable(BuildTable("v"), &cache, 1, &reads);
  ASSERT_NE(table, nullptr);
  auto it = table->NewIterator();
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  cache.Erase({.cache_id = 1, .offset = 0});
  EXPECT_EQ(cache.usage(), 0U);
  EXPECT_EQ(strata::ToString(it->key()), "apple");
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(strata::ToString(it->key()), "banana");
}

}  // namespace

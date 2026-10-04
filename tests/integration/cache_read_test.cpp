#include "strata/db.hpp"
#include "strata/env.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "support/test_dir.hpp"

namespace {

constexpr int kTables = 6;
constexpr int kKeysPerTable = 20;

class CountingFile final : public strata::RandomAccessFile {
 public:
  CountingFile(std::unique_ptr<strata::RandomAccessFile> file, std::atomic<int>* reads)
      : file_(std::move(file)), reads_(reads) {}

  strata::Result<strata::Slice> Read(std::uint64_t offset, std::size_t n,
                                     char* scratch) const override {
    reads_->fetch_add(1);
    return file_->Read(offset, n, scratch);
  }

 private:
  std::unique_ptr<strata::RandomAccessFile> file_;
  std::atomic<int>* reads_;
};

// Counts SSTable opens and reads, so the tests can tell a cache hit from a
// trip to the file.
class CountingEnv final : public strata::EnvWrapper {
 public:
  CountingEnv() : EnvWrapper(strata::Env::Default()) {}

  strata::Result<std::unique_ptr<strata::RandomAccessFile>> NewRandomAccessFile(
      const std::string& path) override {
    STRATA_ASSIGN_OR_RETURN(auto file, target()->NewRandomAccessFile(path));
    if (path.ends_with(".sst")) {
      opens_.fetch_add(1);
      return std::unique_ptr<strata::RandomAccessFile>(
          std::make_unique<CountingFile>(std::move(file), &reads_));
    }
    return file;
  }

  void Reset() {
    opens_.store(0);
    reads_.store(0);
  }
  int opens() const { return opens_.load(); }
  int reads() const { return reads_.load(); }

 private:
  std::atomic<int> opens_{0};
  std::atomic<int> reads_{0};
};

std::string Key(int table, int index) {
  return "t" + std::to_string(table) + "-key" + std::to_string(100 + index);
}

class CacheReadTest : public ::testing::Test {
 protected:
  strata::Options BaseOptions() { return {.path = dir_.File("db"), .env = &env_}; }

  // Each open-write-close cycle leaves one table whose key range is disjoint
  // from the others, so a Get touches exactly one table.
  void WriteTables() {
    for (int table = 0; table < kTables; ++table) {
      strata::DB db;
      ASSERT_TRUE(strata::DB::Open(BaseOptions(), &db).ok());
      for (int index = 0; index < kKeysPerTable; ++index) {
        ASSERT_TRUE(db.Put(Key(table, index), "value" + std::to_string(table)).ok());
      }
      ASSERT_TRUE(db.Close().ok());
    }
  }

  // One Get per table, in table order: with fewer cache slots than tables,
  // every Get evicts the table the next pass will need.
  void ReadOneKeyFromEachTable(strata::DB* db) {
    for (int table = 0; table < kTables; ++table) {
      const auto value = db->Get(Key(table, 0));
      ASSERT_TRUE(value.ok()) << value.status().ToString();
      EXPECT_EQ(*value, "value" + std::to_string(table));
    }
  }

  strata::test::TestDir dir_{"cache_read"};
  CountingEnv env_;
};

// The first pass warms the cache. Opening the database may already have
// opened a table (recovery turns the log into one and verifies it), so only
// the second pass is counted.
TEST_F(CacheReadTest, UnboundedTableCacheKeepsEveryTableOpen) {
  WriteTables();
  strata::DB db;
  ASSERT_TRUE(strata::DB::Open(BaseOptions(), &db).ok());
  ReadOneKeyFromEachTable(&db);
  env_.Reset();
  ReadOneKeyFromEachTable(&db);
  EXPECT_EQ(env_.opens(), 0);
}

TEST_F(CacheReadTest, BoundedTableCacheReopensEvictedTablesAndStaysCorrect) {
  WriteTables();
  strata::Options options = BaseOptions();
  options.max_open_tables = 2;
  strata::DB db;
  ASSERT_TRUE(strata::DB::Open(options, &db).ok());
  ReadOneKeyFromEachTable(&db);
  env_.Reset();
  ReadOneKeyFromEachTable(&db);
  EXPECT_EQ(env_.opens(), kTables) << "cycling through more tables than slots misses every time";
  for (int table = 0; table < kTables; ++table) {
    for (int index = 0; index < kKeysPerTable; ++index) {
      EXPECT_EQ(*db.Get(Key(table, index)), "value" + std::to_string(table));
    }
  }
}

TEST_F(CacheReadTest, BlockCacheServesRepeatedReadsWithoutFileReads) {
  WriteTables();
  strata::DB db;
  ASSERT_TRUE(strata::DB::Open(BaseOptions(), &db).ok());
  ASSERT_EQ(*db.Get(Key(3, 5)), "value3");
  env_.Reset();
  ASSERT_EQ(*db.Get(Key(3, 5)), "value3");
  ASSERT_EQ(*db.Get(Key(3, 6)), "value3") << "same data block";
  EXPECT_EQ(env_.reads(), 0);
}

TEST_F(CacheReadTest, DisabledBlockCacheReadsTheFileEveryTime) {
  WriteTables();
  strata::Options options = BaseOptions();
  options.block_cache_size = 0;
  strata::DB db;
  ASSERT_TRUE(strata::DB::Open(options, &db).ok());
  ASSERT_EQ(*db.Get(Key(3, 5)), "value3");
  env_.Reset();
  ASSERT_EQ(*db.Get(Key(3, 5)), "value3");
  EXPECT_EQ(env_.reads(), 1);
}

}  // namespace

#include "strata/db.hpp"
#include "strata/env.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "support/test_dir.hpp"

namespace {

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

class CountingEnv final : public strata::EnvWrapper {
 public:
  CountingEnv() : EnvWrapper(strata::Env::Default()) {}

  strata::Result<std::unique_ptr<strata::RandomAccessFile>> NewRandomAccessFile(
      const std::string& path) override {
    STRATA_ASSIGN_OR_RETURN(auto file, target()->NewRandomAccessFile(path));
    if (path.ends_with(".sst")) {
      return std::unique_ptr<strata::RandomAccessFile>(
          std::make_unique<CountingFile>(std::move(file), &reads_));
    }
    return file;
  }

  void ResetReads() { reads_.store(0); }
  int reads() const { return reads_.load(); }

 private:
  std::atomic<int> reads_{0};
};

std::string Key(int index) {
  std::string number = std::to_string(index);
  return "key" + std::string(3 - number.size(), '0') + number;
}

TEST(FilterReadTest, DatabaseMissesAvoidSstableBlockReads) {
  strata::test::TestDir dir("filter_read");
  CountingEnv env;
  const strata::Options options{.path = dir.File("db"), .env = &env};
  {
    strata::DB db;
    ASSERT_TRUE(strata::DB::Open(options, &db).ok());
    for (int index = 0; index < 200; index += 2) {
      ASSERT_TRUE(db.Put(Key(index), "value").ok());
    }
    ASSERT_TRUE(db.Close().ok());
  }

  strata::DB reopened;
  ASSERT_TRUE(strata::DB::Open(options, &reopened).ok());
  env.ResetReads();
  for (int index = 1; index < 199; index += 2) {
    EXPECT_TRUE(reopened.Get(Key(index)).status().IsNotFound()) << index;
  }
  EXPECT_LT(env.reads(), 20) << "most absent keys should avoid SSTable reads";
  ASSERT_EQ(*reopened.Get(Key(100)), "value");
}

TEST(FilterReadTest, DisabledFilterMakesMissesReadTheTable) {
  strata::test::TestDir dir("filter_off_read");
  CountingEnv env;
  const strata::Options options{
      .path = dir.File("db"), .bloom_filter = false, .block_cache_size = 0, .env = &env};
  {
    strata::DB db;
    ASSERT_TRUE(strata::DB::Open(options, &db).ok());
    for (int index = 0; index < 200; index += 2) {
      ASSERT_TRUE(db.Put(Key(index), "value").ok());
    }
    ASSERT_TRUE(db.Close().ok());
  }

  strata::DB reopened;
  ASSERT_TRUE(strata::DB::Open(options, &reopened).ok());
  env.ResetReads();
  constexpr int kMisses = 99;
  for (int index = 1; index < 199; index += 2) {
    EXPECT_TRUE(reopened.Get(Key(index)).status().IsNotFound()) << index;
  }
  EXPECT_GE(env.reads(), kMisses) << "without a filter every miss reads a data block";
}

}  // namespace

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "db/db_impl.hpp"
#include "support/file_util.hpp"
#include "support/test_dir.hpp"

namespace {

using strata::DBImpl;
using strata::Options;
using strata::ReadOptions;
using strata::Status;
using strata::WriteBatch;
using strata::WriteOptions;
using strata::test::TestDir;

// Small enough that a few hundred writes fill a memtable.
constexpr std::size_t kSmallWriteBuffer = 16 * 1024;
constexpr std::uint32_t kFillerWrites = 400;

std::string Key(std::uint32_t index) {
  return "key-" + std::to_string(index);
}
std::string Value(std::uint32_t index) {
  return std::string(64, static_cast<char>('a' + index % 26)) + std::to_string(index);
}

class FlushTest : public ::testing::Test {
 protected:
  Options DefaultOptions() const {
    return Options{.path = dir_.File("db"), .write_buffer_size = kSmallWriteBuffer};
  }

  void Open() {
    auto opened = DBImpl::Open(DefaultOptions());
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    db_ = std::move(*opened);
  }
  void Reopen() {
    ASSERT_TRUE(db_->Close().ok());
    db_ = nullptr;
    Open();
  }

  void Put(const std::string& key, const std::string& value) {
    WriteBatch batch;
    batch.Put(key, value);
    ASSERT_TRUE(db_->Write(WriteOptions{}, batch).ok());
  }
  void Delete(const std::string& key) {
    WriteBatch batch;
    batch.Delete(key);
    ASSERT_TRUE(db_->Write(WriteOptions{}, batch).ok());
  }
  std::string Get(const std::string& key) {
    auto value = db_->Get(ReadOptions{}, key);
    if (value.status().IsNotFound()) {
      return "NOT_FOUND";
    }
    return value.ok() ? *value : value.status().ToString();
  }

  // Fills the memtable with unrelated keys until it rotates, then waits for
  // the flush, so everything written before is in a table.
  void ForceFlush() {
    for (std::uint32_t index = 0; index < kFillerWrites; ++index) {
      Put("filler-" + std::to_string(filler_++), std::string(64, 'f'));
    }
    db_->WaitForFlush();
  }

  std::vector<std::string> FilesWithExtension(const std::string& extension) const {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir_.File("db"))) {
      if (entry.path().extension() == extension) {
        names.push_back(entry.path().filename().string());
      }
    }
    std::sort(names.begin(), names.end());
    return names;
  }
  std::vector<std::string> Manifests() const {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir_.File("db"))) {
      if (entry.path().filename().string().starts_with("MANIFEST-")) {
        names.push_back(entry.path().filename().string());
      }
    }
    return names;
  }

  TestDir dir_{"flush_test"};
  std::unique_ptr<DBImpl> db_;
  std::uint32_t filler_ = 0;
};

TEST_F(FlushTest, FullMemtableIsFlushedToATableAndStaysReadable) {
  Open();
  constexpr std::uint32_t kKeys = 2000;
  for (std::uint32_t index = 0; index < kKeys; ++index) {
    Put(Key(index), Value(index));
  }
  db_->WaitForFlush();
  EXPECT_FALSE(FilesWithExtension(".sst").empty());
  for (std::uint32_t index = 0; index < kKeys; ++index) {
    ASSERT_EQ(Get(Key(index)), Value(index)) << index;
  }
  Reopen();
  for (std::uint32_t index = 0; index < kKeys; ++index) {
    ASSERT_EQ(Get(Key(index)), Value(index)) << index;
  }
  const std::string log = strata::test::ReadFileToString(dir_.File("db/LOG"));
  EXPECT_NE(log.find("flushed table"), std::string::npos);
}

TEST_F(FlushTest, MemtableDeleteShadowsAFlushedValue) {
  Open();
  Put("k", "old");
  ForceFlush();
  Delete("k");
  EXPECT_EQ(Get("k"), "NOT_FOUND");
  Put("k", "new");
  EXPECT_EQ(Get("k"), "new");
}

TEST_F(FlushTest, FlushedDeleteShadowsAnOlderTable) {
  Open();
  Put("k", "old");
  ForceFlush();
  Delete("k");
  ForceFlush();
  EXPECT_EQ(Get("k"), "NOT_FOUND");
  Reopen();
  EXPECT_EQ(Get("k"), "NOT_FOUND");
}

// Level-0 tables overlap: the newest must win, before and after a restart.
TEST_F(FlushTest, NewestLevelZeroTableWins) {
  Open();
  Put("k", "first");
  ForceFlush();
  Put("k", "second");
  ForceFlush();
  ASSERT_GE(FilesWithExtension(".sst").size(), 2U);
  EXPECT_EQ(Get("k"), "second");
  Reopen();
  EXPECT_EQ(Get("k"), "second");
}

TEST_F(FlushTest, FlushRetiresTheLogsItCovers) {
  Open();
  ForceFlush();
  ForceFlush();
  EXPECT_EQ(FilesWithExtension(".log").size(), 1U) << "only the active log should remain";
}

TEST_F(FlushTest, RecoveryFlushesReplayedLogs) {
  Open();
  Put("k", "v");
  Reopen();
  EXPECT_EQ(FilesWithExtension(".log").size(), 1U);
  EXPECT_EQ(FilesWithExtension(".sst").size(), 1U);
  EXPECT_EQ(Get("k"), "v");
}

TEST_F(FlushTest, EachSessionLeavesExactlyOneManifest) {
  Open();
  Put("k", "v");
  Reopen();
  Reopen();
  const auto manifests = Manifests();
  ASSERT_EQ(manifests.size(), 1U);
  EXPECT_EQ(strata::test::ReadFileToString(dir_.File("db/CURRENT")), manifests[0] + "\n");
  EXPECT_TRUE(FilesWithExtension(".dbtmp").empty());
}

TEST_F(FlushTest, UnreferencedFilesAreDeletedOnOpen) {
  Open();
  Put("k", "v");
  ASSERT_TRUE(db_->Close().ok());
  db_ = nullptr;
  // What a crash between writing a file and committing it leaves behind.
  std::ofstream(dir_.File("db/000900.sst")) << "orphan table";
  std::ofstream(dir_.File("db/000901.dbtmp")) << "orphan temp";
  Open();
  EXPECT_FALSE(std::filesystem::exists(dir_.File("db/000900.sst")));
  EXPECT_FALSE(std::filesystem::exists(dir_.File("db/000901.dbtmp")));
  EXPECT_EQ(Get("k"), "v");
}

// A table the MANIFEST references but that is gone is corruption, never "key
// not found": that answer would silently lose the data.
TEST_F(FlushTest, MissingTableIsReportedAsCorruption) {
  Open();
  Put("k", "v");
  Reopen();  // the replayed log is flushed to a table
  const auto tables = FilesWithExtension(".sst");
  ASSERT_EQ(tables.size(), 1U);
  ASSERT_TRUE(db_->Close().ok());
  db_ = nullptr;
  std::filesystem::remove(dir_.File("db/" + tables[0]));
  Open();
  const auto value = db_->Get(ReadOptions{}, "k");
  EXPECT_TRUE(value.status().IsCorruption()) << value.status().ToString();
}

TEST_F(FlushTest, CorruptTableIsReportedNotSkipped) {
  Open();
  Put("k", "v");
  Reopen();
  const auto tables = FilesWithExtension(".sst");
  ASSERT_EQ(tables.size(), 1U);
  ASSERT_TRUE(db_->Close().ok());
  db_ = nullptr;
  {
    std::fstream file(dir_.File("db/" + tables[0]),
                      std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(0);
    file.put('\xff');
  }
  Open();
  const auto value = db_->Get(ReadOptions{}, "k");
  EXPECT_TRUE(value.status().IsCorruption()) << value.status().ToString();
}

}  // namespace

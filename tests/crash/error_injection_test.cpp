// Disk-full, read-error, and sync-failure tests. The contract under test:
// an I/O failure is reported as an error, never as missing data or a wrong
// value; it never destroys data that was already safe; and once the fault
// clears, reopening the database recovers everything that was acknowledged.

#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "db/db_impl.hpp"
#include "support/fault_injection_env.hpp"
#include "support/test_dir.hpp"

namespace strata {
namespace {

using test::FaultInjectionEnv;
using test::TestDir;

constexpr int kKeys = 40;

std::string Key(int index) {
  return "key" + std::to_string(100 + index);
}
std::string Value(int index) {
  return std::string(120, static_cast<char>('a' + index % 26)) + std::to_string(index);
}

class ErrorInjectionTest : public ::testing::Test {
 protected:
  Options DbOptions() {
    // No block cache: a cached block would let a read skip the failing disk.
    return Options{.path = dir_.File("db"), .block_cache_size = 0, .env = &env_};
  }

  void Open() {
    auto opened = DBImpl::Open(DbOptions());
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    db_ = std::move(*opened);
  }
  // Closes whatever state the database is in (an error is expected after a
  // fault) and opens it again with the fault cleared.
  void Reopen() {
    static_cast<void>(db_->Close());
    db_ = nullptr;
    Open();
  }

  Status Put(const std::string& key, const std::string& value, bool sync = false) {
    WriteBatch batch;
    batch.Put(key, value);
    return db_->Write(WriteOptions{.sync = sync}, batch);
  }
  void PutKeys(int first, int last, bool sync = false) {
    for (int index = first; index < last; ++index) {
      ASSERT_TRUE(Put(Key(index), Value(index), sync).ok()) << index;
    }
  }
  void ExpectKeys(int first, int last) {
    for (int index = first; index < last; ++index) {
      const auto value = db_->Get(ReadOptions{}, Key(index));
      ASSERT_TRUE(value.ok()) << Key(index) << ": " << value.status().ToString();
      EXPECT_EQ(*value, Value(index));
    }
  }
  std::size_t TableFiles() const {
    std::size_t tables = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir_.File("db"))) {
      if (entry.path().extension() == ".sst") {
        ++tables;
      }
    }
    return tables;
  }
  // Two overlapping level-0 tables, so compacting level 0 is a real merge.
  void MakeTwoOverlappingTables() {
    PutKeys(0, kKeys);
    ASSERT_TRUE(db_->FlushMemTableForTest().ok());
    PutKeys(0, kKeys);
    ASSERT_TRUE(db_->FlushMemTableForTest().ok());
    ASSERT_EQ(db_->FilesAtLevelForTest(0).size(), 2U);
  }

  TestDir dir_{"error_injection"};
  FaultInjectionEnv env_{Env::Default()};
  std::unique_ptr<DBImpl> db_;
};

TEST_F(ErrorInjectionTest, DiskFullInTheLogFailsTheWriteAndKeepsAcknowledgedOnes) {
  Open();
  PutKeys(0, 10, /*sync=*/true);
  env_.SetDiskFull(".log", 10);  // the next record is cut after 10 bytes
  EXPECT_TRUE(Put("big", std::string(500, 'x')).IsIoError());
  EXPECT_TRUE(Put("after", "x").IsIoError()) << "a write after a failed one must not succeed";
  ExpectKeys(0, 10);  // reads keep working

  env_.ClearDiskFull();
  Reopen();
  ExpectKeys(0, 10);
  EXPECT_TRUE(db_->Get(ReadOptions{}, "after").status().IsNotFound());
  EXPECT_TRUE(Put("writable again", "y").ok());
}

TEST_F(ErrorInjectionTest, DiskFullDuringFlushStopsWritesAndLosesNothing) {
  Open();
  PutKeys(0, kKeys);
  env_.SetDiskFull(".sst", 0);
  EXPECT_TRUE(db_->FlushMemTableForTest().IsIoError());
  EXPECT_TRUE(Put("after", "x").IsIoError());
  ExpectKeys(0, kKeys);  // still served from the memtable that failed to flush
  EXPECT_EQ(TableFiles(), 0U) << "the partial table must be deleted";

  env_.ClearDiskFull();
  Reopen();
  ExpectKeys(0, kKeys);
}

TEST_F(ErrorInjectionTest, DiskFullDuringCompactionKeepsItsInputs) {
  Open();
  MakeTwoOverlappingTables();
  env_.SetDiskFull(".sst", 0);
  EXPECT_TRUE(db_->CompactLevelForTest(0).IsIoError());
  EXPECT_EQ(db_->FilesAtLevelForTest(0).size(), 2U);
  EXPECT_EQ(TableFiles(), 2U) << "outputs are deleted, inputs kept";
  ExpectKeys(0, kKeys);

  env_.ClearDiskFull();
  Reopen();
  ExpectKeys(0, kKeys);
  EXPECT_TRUE(db_->CompactLevelForTest(0).ok()) << "compaction succeeds once there is space";
  ExpectKeys(0, kKeys);
}

TEST_F(ErrorInjectionTest, ReadErrorIsReportedNotTreatedAsMissing) {
  Open();
  PutKeys(0, kKeys);
  ASSERT_TRUE(db_->FlushMemTableForTest().ok());
  ASSERT_TRUE(Put("in memory", "m").ok());
  env_.SetReadFailure(".sst");

  const auto failed = db_->Get(ReadOptions{}, Key(5));
  EXPECT_TRUE(failed.status().IsIoError()) << failed.status().ToString();
  EXPECT_EQ(*db_->Get(ReadOptions{}, "in memory"), "m");

  env_.SetReadFailure(std::nullopt);
  ExpectKeys(0, kKeys);
}

TEST_F(ErrorInjectionTest, ReadErrorDuringCompactionKeepsItsInputs) {
  Open();
  MakeTwoOverlappingTables();
  for (const FileMetaData& file : db_->FilesAtLevelForTest(0)) {
    db_->EvictTableForTest(file.number);
  }
  env_.SetReadFailure(".sst");
  EXPECT_TRUE(db_->CompactLevelForTest(0).IsIoError());
  env_.SetReadFailure(std::nullopt);
  EXPECT_EQ(db_->FilesAtLevelForTest(0).size(), 2U);
  ExpectKeys(0, kKeys);

  Reopen();
  ExpectKeys(0, kKeys);
}

TEST_F(ErrorInjectionTest, SyncFailureDuringCompactionKeepsItsInputs) {
  Open();
  MakeTwoOverlappingTables();
  env_.SetSyncFailure(true);
  EXPECT_TRUE(db_->CompactLevelForTest(0).IsIoError());
  env_.SetSyncFailure(false);
  EXPECT_EQ(db_->FilesAtLevelForTest(0).size(), 2U);
  ExpectKeys(0, kKeys);

  Reopen();
  ExpectKeys(0, kKeys);
  EXPECT_TRUE(db_->CompactLevelForTest(0).ok());
  ExpectKeys(0, kKeys);
}

}  // namespace
}  // namespace strata

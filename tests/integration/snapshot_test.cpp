#include "strata/snapshot.hpp"

#include "strata/db.hpp"

#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "support/test_dir.hpp"

namespace {

// Small enough that a few hundred writes rotate the memtable to a table.
constexpr std::size_t kSmallWriteBuffer = 16 * 1024;
constexpr int kFillerWrites = 600;

class SnapshotTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(
        strata::DB::Open({.path = dir_.File("db"), .write_buffer_size = kSmallWriteBuffer}, &db_)
            .ok());
  }

  std::string Get(const std::string& key, const strata::Snapshot* snapshot = nullptr) {
    const auto value = db_.Get(key, {.snapshot = snapshot});
    if (value.status().IsNotFound()) {
      return "NOT_FOUND";
    }
    return value.ok() ? *value : value.status().ToString();
  }

  // Enough unrelated writes that earlier ones end up in an SSTable.
  void PushToTables() {
    for (int index = 0; index < kFillerWrites; ++index) {
      ASSERT_TRUE(db_.Put("filler" + std::to_string(index), std::string(64, 'f')).ok());
    }
  }

  strata::test::TestDir dir_{"snapshot"};
  strata::DB db_;
};

TEST_F(SnapshotTest, SnapshotKeepsSeeingTheValuesOfItsMoment) {
  ASSERT_TRUE(db_.Put("key", "v1").ok());
  const strata::Snapshot* snapshot = db_.GetSnapshot();
  ASSERT_NE(snapshot, nullptr);
  ASSERT_TRUE(db_.Put("key", "v2").ok());
  ASSERT_TRUE(db_.Put("new", "x").ok());
  ASSERT_TRUE(db_.Delete("gone-later").ok());

  EXPECT_EQ(Get("key", snapshot), "v1");
  EXPECT_EQ(Get("new", snapshot), "NOT_FOUND");
  EXPECT_EQ(Get("key"), "v2");
  EXPECT_TRUE(db_.ReleaseSnapshot(snapshot).ok());
}

TEST_F(SnapshotTest, SnapshotStillSeesADeletedKey) {
  ASSERT_TRUE(db_.Put("key", "kept").ok());
  const strata::Snapshot* snapshot = db_.GetSnapshot();
  ASSERT_TRUE(db_.Delete("key").ok());
  EXPECT_EQ(Get("key", snapshot), "kept");
  EXPECT_EQ(Get("key"), "NOT_FOUND");
  EXPECT_TRUE(db_.ReleaseSnapshot(snapshot).ok());
}

TEST_F(SnapshotTest, SnapshotReadsOldVersionsFromTables) {
  ASSERT_TRUE(db_.Put("key", "v1").ok());
  const strata::Snapshot* snapshot = db_.GetSnapshot();
  ASSERT_TRUE(db_.Put("key", "v2").ok());
  PushToTables();
  EXPECT_EQ(Get("key", snapshot), "v1");
  EXPECT_EQ(Get("key"), "v2");
  EXPECT_TRUE(db_.ReleaseSnapshot(snapshot).ok());
}

TEST_F(SnapshotTest, ReleasedOrForeignSnapshotsAreRejected) {
  const strata::Snapshot* snapshot = db_.GetSnapshot();
  ASSERT_TRUE(db_.ReleaseSnapshot(snapshot).ok());
  EXPECT_TRUE(db_.ReleaseSnapshot(snapshot).IsInvalidArgument());
  EXPECT_TRUE(db_.Get("key", {.snapshot = snapshot}).status().IsInvalidArgument());
  EXPECT_TRUE(db_.ReleaseSnapshot(nullptr).IsInvalidArgument());
}

TEST_F(SnapshotTest, ClosedDatabaseHandsOutNoSnapshot) {
  ASSERT_TRUE(db_.Close().ok());
  EXPECT_EQ(db_.GetSnapshot(), nullptr);
}

}  // namespace

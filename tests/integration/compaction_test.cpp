#include "strata/db.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "db/db_impl.hpp"
#include "db/dbformat.hpp"
#include "db/filename.hpp"
#include "support/test_dir.hpp"

namespace strata {
namespace {

using test::TestDir;

constexpr int kStopWritesTrigger = 12;

class CompactionTest : public ::testing::Test {
 protected:
  Options DefaultOptions() const { return Options{.path = dir_.File("db")}; }

  void Open(const Options& options) {
    auto opened = DBImpl::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    db_ = std::move(*opened);
  }
  void Open() { Open(DefaultOptions()); }
  void Reopen(const Options& options) {
    ASSERT_TRUE(db_->Close().ok());
    db_ = nullptr;
    Open(options);
  }
  void Reopen() { Reopen(DefaultOptions()); }

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
  std::string Get(const std::string& key, const Snapshot* snapshot = nullptr) {
    auto value = db_->Get(ReadOptions{.snapshot = snapshot}, key);
    if (value.status().IsNotFound()) {
      return "NOT_FOUND";
    }
    return value.ok() ? *value : value.status().ToString();
  }
  void Flush() { ASSERT_TRUE(db_->FlushMemTableForTest().ok()); }
  void CompactLevel(int level) {
    const Status status = db_->CompactLevelForTest(level);
    ASSERT_TRUE(status.ok()) << status.ToString();
  }
  std::size_t Files(int level) { return db_->FilesAtLevelForTest(level).size(); }

  // Writes `keys` into one table and pushes it down to `level`. A table that
  // overlaps nothing below it is moved rather than rewritten, so tests seed an
  // overlapping table first whenever they need the merge itself to run.
  void SeedLevel(int level, const std::vector<std::string>& keys) {
    for (const std::string& key : keys) {
      Put(key, "seed");
    }
    Flush();
    for (int from = 0; from < level; ++from) {
      CompactLevel(from);
    }
  }

  TestDir dir_{"compaction_test"};
  std::unique_ptr<DBImpl> db_;
};

TEST_F(CompactionTest, LevelZeroCompactsIntoLevelOneAtItsTrigger) {
  Open();
  for (int table = 0; table < 4; ++table) {
    for (int index = 0; index < 50; ++index) {
      Put("key" + std::to_string(index), "value" + std::to_string(table));
    }
    Flush();
  }
  db_->WaitForCompactionsForTest();
  EXPECT_EQ(Files(0), 0U);
  EXPECT_GE(Files(1), 1U);
  for (int index = 0; index < 50; ++index) {
    EXPECT_EQ(Get("key" + std::to_string(index)), "value3");
  }
  Reopen();
  EXPECT_EQ(Get("key7"), "value3");
}

// The phase gate. A tombstone that reaches a level above an older value of its
// key must survive compaction until it meets that value; dropping it early
// would bring the deleted value back.
TEST_F(CompactionTest, CompactionDoesNotResurrectDeletedKey) {
  Open();
  Put("doomed", "old value");
  Put("other", "kept");
  Flush();
  CompactLevel(0);
  CompactLevel(1);
  ASSERT_EQ(Files(2), 1U) << "the old value now lives in level 2";

  // A level-1 table around "doomed" makes the next compaction a real merge
  // rather than a move, so the tombstone-drop rule is what gets tested.
  SeedLevel(1, {"d", "e"});
  Delete("doomed");
  Flush();
  CompactLevel(0);
  ASSERT_EQ(Files(1), 1U) << "the tombstone now sits in level 1, above level 2";
  EXPECT_EQ(Get("doomed"), "NOT_FOUND");
  Reopen();
  EXPECT_EQ(Get("doomed"), "NOT_FOUND");

  CompactLevel(1);
  EXPECT_EQ(Files(1), 0U);
  EXPECT_EQ(Get("doomed"), "NOT_FOUND");
  EXPECT_EQ(Get("other"), "kept");
  Reopen();
  EXPECT_EQ(Get("doomed"), "NOT_FOUND");
  EXPECT_EQ(Get("other"), "kept");
}

TEST_F(CompactionTest, ValueAndTombstoneBothVanishAtTheBottom) {
  Open();
  SeedLevel(1, {"a", "z"});
  const std::string big(10'000, 'g');
  Put("gone", big);
  Delete("gone");
  Flush();
  CompactLevel(0);
  EXPECT_EQ(Files(0), 0U);
  EXPECT_LT(db_->LevelBytesForTest(1), big.size()) << "neither the value nor its tombstone stays";
  EXPECT_EQ(Get("gone"), "NOT_FOUND");
}

TEST_F(CompactionTest, SnapshotKeepsAnOverwrittenValueThroughCompaction) {
  Open();
  SeedLevel(2, {"k", "l"});
  const std::string big_old(10'000, 'o');
  Put("key", big_old);
  const Snapshot* snapshot = db_->GetSnapshot();
  Put("key", "new");
  Flush();
  CompactLevel(0);
  EXPECT_EQ(Get("key", snapshot), big_old);
  EXPECT_EQ(Get("key"), "new");
  EXPECT_GT(db_->LevelBytesForTest(1), big_old.size());

  ASSERT_TRUE(db_->ReleaseSnapshot(snapshot).ok());
  CompactLevel(1);
  EXPECT_EQ(Files(1), 0U);
  EXPECT_EQ(Get("key"), "new");
  EXPECT_LT(db_->LevelBytesForTest(2), big_old.size()) << "the old value was reclaimed";
}

TEST_F(CompactionTest, SnapshotKeepsADeletedValueThroughCompaction) {
  Open();
  SeedLevel(1, {"a", "z"});
  Put("key", "before delete");
  const Snapshot* snapshot = db_->GetSnapshot();
  Delete("key");
  Flush();
  CompactLevel(0);
  EXPECT_EQ(Get("key", snapshot), "before delete");
  EXPECT_EQ(Get("key"), "NOT_FOUND");
  ASSERT_TRUE(db_->ReleaseSnapshot(snapshot).ok());
}

TEST_F(CompactionTest, OverwrittenVersionsAreReclaimed) {
  Open();
  const std::string value(1000, 'v');
  for (int round = 0; round < 5; ++round) {
    for (int index = 0; index < 20; ++index) {
      Put("key" + std::to_string(index), value + std::to_string(round));
    }
    Flush();
  }
  CompactLevel(0);
  EXPECT_LT(db_->LevelBytesForTest(1), 2U * 20U * value.size()) << "only the newest round remains";
  EXPECT_EQ(Get("key3"), value + "4");
}

TEST_F(CompactionTest, OutputsNeverSplitOneUserKeyAcrossTables) {
  Options options = DefaultOptions();
  options.max_file_size = 1024;
  Open(options);
  SeedLevel(1, {"a", "z"});
  std::vector<const Snapshot*> snapshots;
  for (int version = 0; version < 20; ++version) {
    Put("hot", std::string(200, static_cast<char>('a' + version)));
    snapshots.push_back(db_->GetSnapshot());
  }
  for (int index = 0; index < 20; ++index) {
    Put("cold" + std::to_string(index), std::string(200, 'c'));
  }
  Flush();
  CompactLevel(0);
  EXPECT_GE(Files(1), 2U) << "small max_file_size forces several outputs";
  for (int version = 0; version < 20; ++version) {
    EXPECT_EQ(Get("hot", snapshots[static_cast<std::size_t>(version)]),
              std::string(200, static_cast<char>('a' + version)));
  }
  for (const Snapshot* snapshot : snapshots) {
    ASSERT_TRUE(db_->ReleaseSnapshot(snapshot).ok());
  }
}

TEST_F(CompactionTest, ASingleFileWithNoOverlapBelowIsMovedNotRewritten) {
  Open();
  Put("a", "1");
  Flush();
  CompactLevel(0);
  const auto before = db_->FilesAtLevelForTest(1);
  ASSERT_EQ(before.size(), 1U);
  CompactLevel(1);
  const auto after = db_->FilesAtLevelForTest(2);
  ASSERT_EQ(after.size(), 1U);
  EXPECT_EQ(after[0].number, before[0].number);
  EXPECT_EQ(Get("a"), "1");
}

TEST_F(CompactionTest, RandomWorkloadMatchesAReferenceModel) {
  Options options = DefaultOptions();
  options.write_buffer_size = 8 * 1024;
  options.max_file_size = 4 * 1024;
  options.max_bytes_for_level_base = 16 * 1024;
  Open(options);
  std::mt19937 random(42);
  std::map<std::string, std::string> model;
  for (int step = 0; step < 4000; ++step) {
    const std::string key = "k" + std::to_string(random() % 300);
    const auto action = random() % 100;
    if (action < 60) {
      const std::string value = std::string(random() % 200, 'x') + std::to_string(step);
      Put(key, value);
      model[key] = value;
    } else if (action < 90) {
      Delete(key);
      model.erase(key);
    } else if (action < 97) {
      CompactLevel(static_cast<int>(random() % 3));
    } else {
      Reopen(options);
    }
  }
  db_->WaitForCompactionsForTest();
  for (int index = 0; index < 300; ++index) {
    const std::string key = "k" + std::to_string(index);
    const auto found = model.find(key);
    EXPECT_EQ(Get(key), found == model.end() ? "NOT_FOUND" : found->second) << key;
  }
}

TEST_F(CompactionTest, BackgroundCompactionKeepsLevelZeroSmall) {
  Options options = DefaultOptions();
  options.write_buffer_size = 8 * 1024;
  Open(options);
  for (int index = 0; index < 3000; ++index) {
    Put("key" + std::to_string(index % 500), std::string(100, 'v') + std::to_string(index));
  }
  db_->WaitForCompactionsForTest();
  EXPECT_LT(Files(0), 4U);
  EXPECT_EQ(Get("key499"), std::string(100, 'v') + "2999");
}

TEST_F(CompactionTest, WritesSlowDownAndStopWhenLevelZeroPilesUp) {
  Options options = DefaultOptions();
  options.write_buffer_size = 4 * 1024;
  Open(options);
  db_->PauseCompactionsForTest(true);
  for (int table = 0; table < kStopWritesTrigger; ++table) {
    Put("key" + std::to_string(table), "v");
    Flush();
  }
  ASSERT_EQ(Files(0), static_cast<std::size_t>(kStopWritesTrigger));
  const std::uint64_t slowdowns_before = db_->GetStats().write_slowdowns;
  Put("one more", "v");
  EXPECT_GT(db_->GetStats().write_slowdowns, slowdowns_before);

  // With the memtable full and level 0 at the stop trigger, a writer waits.
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (int index = 0; index < 200; ++index) {
      Put("fill" + std::to_string(index), std::string(100, 'f'));
    }
    done = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_FALSE(done.load()) << "writes must stop at the stop trigger";
  db_->PauseCompactionsForTest(false);
  writer.join();
  EXPECT_TRUE(done.load());
  EXPECT_EQ(Get("fill199"), std::string(100, 'f'));
}

TEST_F(CompactionTest, CorruptInputFailsTheCompactionAndKeepsTheInputs) {
  Open();
  SeedLevel(1, {"key", "keyz"});
  for (int index = 0; index < 200; ++index) {
    Put("key" + std::to_string(index), std::string(100, 'v'));
  }
  Flush();
  const auto files = db_->FilesAtLevelForTest(0);
  ASSERT_EQ(files.size(), 1U);
  const std::string table = TableFileName(dir_.File("db"), files[0].number);
  {
    // Flip a byte in the first data block; its checksum no longer matches.
    std::fstream file(table, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(10);
    file.put('\xFF');
  }
  db_->EvictTableForTest(files[0].number);
  EXPECT_TRUE(db_->CompactLevelForTest(0).IsCorruption());
  EXPECT_TRUE(std::filesystem::exists(table)) << "a failed compaction must not delete its inputs";
  EXPECT_EQ(Files(0), 1U);
}

}  // namespace
}  // namespace strata

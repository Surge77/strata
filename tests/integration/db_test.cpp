#include "strata/db.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "db/filename.hpp"
#include "db/write_batch_internal.hpp"
#include "log/log_writer.hpp"
#include "support/file_util.hpp"
#include "support/test_dir.hpp"

namespace {

using strata::DB;
using strata::Options;
using strata::Slice;
using strata::Status;
using strata::WriteBatch;
using strata::test::TestDir;

class DBTest : public ::testing::Test {
 protected:
  Options DefaultOptions() const { return Options{.path = dir_.File("db")}; }

  void Open(Options options) {
    const Status status = DB::Open(options, &db_);
    ASSERT_TRUE(status.ok()) << status.ToString();
  }
  void Open() { Open(DefaultOptions()); }

  void Reopen() {
    ASSERT_TRUE(db_.Close().ok());
    Open();
  }

  std::string Get(Slice key) {
    auto value = db_.Get(key);
    if (value.status().IsNotFound()) {
      return "NOT_FOUND";
    }
    EXPECT_TRUE(value.ok()) << value.status().ToString();
    return value.ok() ? *value : value.status().ToString();
  }

  std::vector<std::filesystem::path> LogFiles() const {
    std::vector<std::filesystem::path> logs;
    for (const auto& entry : std::filesystem::directory_iterator(dir_.File("db"))) {
      if (entry.path().extension() == ".log") {
        logs.push_back(entry.path());
      }
    }
    std::sort(logs.begin(), logs.end());
    return logs;
  }

  TestDir dir_{"db"};
  DB db_;
};

TEST_F(DBTest, PutGetDelete) {
  Open();
  ASSERT_TRUE(db_.Put("user:101", "rahul").ok());
  EXPECT_EQ(Get("user:101"), "rahul");
  ASSERT_TRUE(db_.Put("user:101", "priya").ok());
  EXPECT_EQ(Get("user:101"), "priya");
  ASSERT_TRUE(db_.Delete("user:101").ok());
  EXPECT_EQ(Get("user:101"), "NOT_FOUND");
  EXPECT_TRUE(db_.Delete("never-existed").ok());
}

TEST_F(DBTest, MissingKeyIsNotFound) {
  Open();
  EXPECT_TRUE(db_.Get("absent").status().IsNotFound());
}

TEST_F(DBTest, EmptyKeysAndBinaryValuesWork) {
  Open();
  const std::string binary("\0\x01\xff", 3);
  ASSERT_TRUE(db_.Put("", binary).ok());
  EXPECT_EQ(Get(""), binary);
}

TEST_F(DBTest, WriteBatchAppliesInOrder) {
  Open();
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  batch.Delete("a");
  batch.Put("c", "3");
  ASSERT_TRUE(db_.Write(batch).ok());
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  EXPECT_EQ(Get("b"), "2");
  EXPECT_EQ(Get("c"), "3");
  EXPECT_TRUE(db_.Write(WriteBatch()).ok());  // an empty batch is a no-op
}

TEST_F(DBTest, DataSurvivesCloseAndReopen) {
  Open();
  ASSERT_TRUE(db_.Put("k1", "v1").ok());
  ASSERT_TRUE(db_.Put("k2", "v2").ok());
  ASSERT_TRUE(db_.Delete("k1").ok());
  Reopen();
  EXPECT_EQ(Get("k1"), "NOT_FOUND");
  EXPECT_EQ(Get("k2"), "v2");
}

// Sequence numbers must continue across reopens: a write made after reopening
// must beat an older value recovered from an earlier log.
TEST_F(DBTest, WritesAfterReopenOverrideRecoveredOnes) {
  Open();
  ASSERT_TRUE(db_.Put("k", "old").ok());
  Reopen();
  ASSERT_TRUE(db_.Put("k", "new").ok());
  Reopen();
  EXPECT_EQ(Get("k"), "new");
  ASSERT_TRUE(db_.Delete("k").ok());
  Reopen();
  EXPECT_EQ(Get("k"), "NOT_FOUND");
}

TEST_F(DBTest, ManyReopensKeepEverything) {
  std::map<std::string, std::string> model;
  for (int round = 0; round < 10; ++round) {
    Open();
    for (int index = 0; index < 50; ++index) {
      const std::string key = "k" + std::to_string((round * 37 + index) % 120);
      const std::string value = "r" + std::to_string(round) + "i" + std::to_string(index);
      ASSERT_TRUE(db_.Put(key, value).ok());
      model[key] = value;
    }
    ASSERT_TRUE(db_.Close().ok());
  }
  Open();
  for (const auto& [key, value] : model) {
    ASSERT_EQ(Get(key), value) << key;
  }
  EXPECT_EQ(LogFiles().size(), 11U) << "phase 3 keeps every log until flush exists";
}

// Differential test against std::map: random puts, deletes, and batches, with
// reopens in between. This is the phase-3 merge gate "differential test".
TEST_F(DBTest, AgreesWithStdMapAcrossReopens) {
  std::mt19937 random(42);
  std::map<std::string, std::string> model;
  Open();
  for (int step = 0; step < 4000; ++step) {
    const std::string key = "key" + std::to_string(random() % 300);
    const int action = static_cast<int>(random() % 10);
    if (action < 6) {
      const std::string value = std::string(random() % 64, static_cast<char>('a' + step % 26));
      ASSERT_TRUE(db_.Put(key, value).ok());
      model[key] = value;
    } else if (action < 8) {
      ASSERT_TRUE(db_.Delete(key).ok());
      model.erase(key);
    } else if (action < 9) {
      WriteBatch batch;
      const std::string other = "key" + std::to_string(random() % 300);
      batch.Put(key, "batched");
      batch.Delete(other);
      ASSERT_TRUE(db_.Write(batch).ok());
      model[key] = "batched";
      model.erase(other);
    } else {
      const auto found = model.find(key);
      ASSERT_EQ(Get(key), found == model.end() ? "NOT_FOUND" : found->second) << key;
    }
    if (step % 1000 == 999) {
      Reopen();
    }
  }
  Reopen();
  for (int index = 0; index < 300; ++index) {
    const std::string key = "key" + std::to_string(index);
    const auto found = model.find(key);
    ASSERT_EQ(Get(key), found == model.end() ? "NOT_FOUND" : found->second) << key;
  }
}

TEST_F(DBTest, OpenFailsWithoutCreateIfMissing) {
  Options options = DefaultOptions();
  options.create_if_missing = false;
  EXPECT_TRUE(DB::Open(options, &db_).IsInvalidArgument());
  EXPECT_FALSE(db_.is_open());
}

TEST_F(DBTest, ErrorIfExistsRejectsAnExistingDatabase) {
  Open();
  ASSERT_TRUE(db_.Close().ok());
  Options options = DefaultOptions();
  options.error_if_exists = true;
  EXPECT_EQ(DB::Open(options, &db_).code(), Status::Code::kAlreadyExists);
}

TEST_F(DBTest, EmptyPathIsRejected) {
  EXPECT_TRUE(DB::Open(Options{}, &db_).IsInvalidArgument());
}

TEST_F(DBTest, SecondOpenOfTheSameDatabaseIsBusy) {
  Open();
  DB second;
  EXPECT_TRUE(DB::Open(DefaultOptions(), &second).IsBusy());
  ASSERT_TRUE(db_.Close().ok());
  EXPECT_TRUE(DB::Open(DefaultOptions(), &second).ok()) << "Close must release the lock";
}

TEST_F(DBTest, OpeningAnAlreadyOpenHandleIsRejected) {
  Open();
  EXPECT_TRUE(DB::Open(DefaultOptions(), &db_).IsInvalidArgument());
  EXPECT_TRUE(db_.is_open());
}

TEST_F(DBTest, ClosedAndMovedFromHandlesFailCleanly) {
  DB closed;
  EXPECT_TRUE(closed.Put("k", "v").IsInvalidArgument());
  EXPECT_TRUE(closed.Delete("k").IsInvalidArgument());
  EXPECT_TRUE(closed.Get("k").status().IsInvalidArgument());
  EXPECT_TRUE(closed.Close().ok());

  Open();
  ASSERT_TRUE(db_.Put("k", "v").ok());
  DB moved = std::move(db_);
  EXPECT_FALSE(db_.is_open());  // NOLINT(bugprone-use-after-move): the point of the test
  EXPECT_EQ(*moved.Get("k"), "v");

  DB assigned;
  assigned = std::move(moved);
  EXPECT_EQ(*assigned.Get("k"), "v");
}

// Keys and values beyond the format's varint32 limits are refused, never
// truncated (ADR-0005). The Slices claim huge sizes over a one-byte buffer;
// the size check must reject them before a single byte is read.
TEST_F(DBTest, OversizedKeysAndValuesAreRejected) {
  Open();
  const char byte = 'x';
  const Slice huge_key(&byte, WriteBatch::kMaxKeySize + 1ULL);
  const Slice huge_value(&byte, WriteBatch::kMaxValueSize + 1ULL);
  EXPECT_TRUE(db_.Put(huge_key, "v").IsInvalidArgument());
  EXPECT_TRUE(db_.Put("k", huge_value).IsInvalidArgument());
  EXPECT_TRUE(db_.Delete(huge_key).IsInvalidArgument());
  EXPECT_TRUE(db_.Get(huge_key).status().IsInvalidArgument());
  ASSERT_TRUE(db_.Put("still", "works").ok());
  EXPECT_EQ(Get("still"), "works");
}

TEST_F(DBTest, WritesTheInfoLogAndKeepsThePreviousOne) {
  Open();
  Reopen();
  EXPECT_TRUE(std::filesystem::exists(dir_.File("db/LOG")));
  EXPECT_TRUE(std::filesystem::exists(dir_.File("db/LOG.old")));
  const std::string contents = strata::test::ReadFileToString(dir_.File("db/LOG"));
  EXPECT_NE(contents.find("replayed"), std::string::npos) << contents;
}

// A torn record at the end of the WAL is what a crash leaves behind: recovery
// must discard it quietly and keep everything before it.
TEST_F(DBTest, TornTailIsDiscardedSilently) {
  Open();
  ASSERT_TRUE(db_.Put("kept", "yes").ok());
  ASSERT_TRUE(db_.Put("torn", std::string(1000, 't')).ok());
  ASSERT_TRUE(db_.Close().ok());
  // One Open, one log: a fresh log is only created by the next Open.
  const auto logs = LogFiles();
  ASSERT_EQ(logs.size(), 1U);
  const auto& last_with_data = logs.front();
  std::filesystem::resize_file(last_with_data, std::filesystem::file_size(last_with_data) - 10);
  Open();
  EXPECT_EQ(Get("kept"), "yes");
  EXPECT_EQ(Get("torn"), "NOT_FOUND");
}

// Damage in the middle of the WAL fails Open by default (ADR-0005)...
TEST_F(DBTest, CorruptionInsideTheLogFailsOpen) {
  Open();
  ASSERT_TRUE(db_.Put("first", "1").ok());
  ASSERT_TRUE(db_.Put("second", "2").ok());
  ASSERT_TRUE(db_.Close().ok());
  const auto logs = LogFiles();
  ASSERT_EQ(logs.size(), 1U);
  {
    std::fstream file(logs.front(), std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(12);  // inside the first record's payload
    file.put('\x7f');
  }
  const Status status = DB::Open(DefaultOptions(), &db_);
  EXPECT_TRUE(status.IsCorruption()) << status.ToString();
  EXPECT_NE(status.message().find(".log"), Slice::npos) << "the error must name the file";
  EXPECT_FALSE(db_.is_open());
}

// ...and salvage mode opens anyway, dropping the damaged block.
TEST_F(DBTest, IgnoreWalCorruptionSalvagesTheRest) {
  Open();
  ASSERT_TRUE(db_.Put("damaged", "1").ok());
  ASSERT_TRUE(db_.Close().ok());
  Open();
  ASSERT_TRUE(db_.Put("later", "2").ok());  // in the next log file
  ASSERT_TRUE(db_.Close().ok());
  const auto logs = LogFiles();
  {
    std::fstream file(logs[0], std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(12);
    file.put('\x7f');
  }
  Options options = DefaultOptions();
  options.ignore_wal_corruption = true;
  Open(options);
  EXPECT_EQ(Get("damaged"), "NOT_FOUND");
  EXPECT_EQ(Get("later"), "2");
}

// A log record can pass its checksum and still not be a WriteBatch -- a bug
// in a writer, or a log from another program. Recovery must treat it as
// corruption: fail by default, skip it when salvaging.
class RecoveryOfNonBatches : public DBTest {
 protected:
  void WriteLogWithBadRecords() {
    ASSERT_TRUE(std::filesystem::create_directories(dir_.File("db")));
    auto file = strata::Env::Default()->NewWritableFile(strata::LogFileName(dir_.File("db"), 1));
    ASSERT_TRUE(file.ok());
    strata::log::Writer writer(file->get());
    WriteBatch good;
    good.Put("good", "yes");
    strata::WriteBatchInternal::SetSequence(&good, 1);
    ASSERT_TRUE(writer.AddRecord("tiny").ok());  // shorter than a batch header
    std::string lying(strata::WriteBatchInternal::Contents(good));
    lying[8] = 5;  // count claims 5 operations; the batch holds 1
    ASSERT_TRUE(writer.AddRecord(lying).ok());
    ASSERT_TRUE(writer.AddRecord(strata::WriteBatchInternal::Contents(good)).ok());
    ASSERT_TRUE((*file)->Close().ok());
  }
};

TEST_F(RecoveryOfNonBatches, FailOpenByDefault) {
  WriteLogWithBadRecords();
  const Status status = DB::Open(DefaultOptions(), &db_);
  EXPECT_TRUE(status.IsCorruption()) << status.ToString();
}

TEST_F(RecoveryOfNonBatches, AreSkippedWhenSalvaging) {
  WriteLogWithBadRecords();
  Options options = DefaultOptions();
  options.ignore_wal_corruption = true;
  Open(options);
  EXPECT_EQ(Get("good"), "yes");
  const std::string log = strata::test::ReadFileToString(dir_.File("db/LOG"));
  EXPECT_NE(log.find("shorter than a batch"), std::string::npos) << log;
  EXPECT_NE(log.find("count does not match"), std::string::npos) << log;
}

// Files that are not the database's own are neither recovered nor touched.
TEST_F(DBTest, StrayFilesAreIgnored) {
  Open();
  ASSERT_TRUE(db_.Put("k", "v").ok());
  ASSERT_TRUE(db_.Close().ok());
  {
    std::ofstream stray(dir_.File("db/notes.txt"));
    stray << "not ours";
  }
  Open();
  EXPECT_EQ(Get("k"), "v");
  EXPECT_TRUE(std::filesystem::exists(dir_.File("db/notes.txt")));
}

TEST_F(DBTest, ConcurrentWritersAndReadersAreSafe) {
  Open();
  constexpr int kThreads = 4;
  constexpr int kWritesPerThread = 500;
  std::vector<std::thread> threads;
  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([this, thread] {
      for (int index = 0; index < kWritesPerThread; ++index) {
        const std::string key = "t" + std::to_string(thread) + "-" + std::to_string(index);
        ASSERT_TRUE(db_.Put(key, key).ok());
        ASSERT_EQ(*db_.Get(key), key);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  Reopen();
  for (int thread = 0; thread < kThreads; ++thread) {
    for (int index = 0; index < kWritesPerThread; index += 50) {
      const std::string key = "t" + std::to_string(thread) + "-" + std::to_string(index);
      ASSERT_EQ(Get(key), key);
    }
  }
}

}  // namespace

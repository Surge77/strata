#include "strata/db.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "db/db_impl.hpp"
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
  EXPECT_EQ(LogFiles().size(), 1U) << "recovery consolidates every log into one";
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

// The info log is appended to, never rotated: rotating on every open would
// lose the log of a crashed session after two failed reopen attempts.
TEST_F(DBTest, InfoLogAccumulatesAcrossSessions) {
  Open();
  Reopen();
  EXPECT_FALSE(std::filesystem::exists(dir_.File("db/LOG.old")));
  const std::string contents = strata::test::ReadFileToString(dir_.File("db/LOG"));
  std::size_t banners = 0;
  for (std::size_t at = contents.find("=== opening"); at != std::string::npos;
       at = contents.find("=== opening", at + 1)) {
    ++banners;
  }
  EXPECT_EQ(banners, 2U) << contents;
  EXPECT_NE(contents.find("consolidated"), std::string::npos) << contents;
}

// Rejecting an existing database must leave it exactly as it was.
TEST_F(DBTest, ErrorIfExistsWritesNothing) {
  Open();
  ASSERT_TRUE(db_.Close().ok());
  const std::string log_before = strata::test::ReadFileToString(dir_.File("db/LOG"));
  Options options = DefaultOptions();
  options.error_if_exists = true;
  ASSERT_EQ(DB::Open(options, &db_).code(), Status::Code::kAlreadyExists);
  EXPECT_EQ(strata::test::ReadFileToString(dir_.File("db/LOG")), log_before);
}

// A file numbered UINT64_MAX would make the next file number wrap to 0.
TEST_F(DBTest, ExhaustedFileNumbersRefuseToOpen) {
  ASSERT_TRUE(std::filesystem::create_directories(dir_.File("db")));
  std::ofstream(dir_.File("db/18446744073709551615.log")) << "";
  EXPECT_TRUE(DB::Open(DefaultOptions(), &db_).IsCorruption());
}

// Overwrites a byte of the only log file.
void DamageLog(const std::filesystem::path& log, std::streamoff offset) {
  std::fstream file(log, std::ios::in | std::ios::out | std::ios::binary);
  file.seekp(offset);
  file.put('\x7f');
}

// A torn record at the end of the WAL is what a crash leaves behind: recovery
// must discard it quietly and keep everything before it.
TEST_F(DBTest, TornTailIsDiscardedSilently) {
  Open();
  ASSERT_TRUE(db_.Put("kept", "yes").ok());
  ASSERT_TRUE(db_.Put("torn", std::string(1000, 't')).ok());
  ASSERT_TRUE(db_.Close().ok());
  const auto logs = LogFiles();
  ASSERT_EQ(logs.size(), 1U);
  std::filesystem::resize_file(logs.front(), std::filesystem::file_size(logs.front()) - 10);
  Open();
  EXPECT_EQ(Get("kept"), "yes");
  EXPECT_EQ(Get("torn"), "NOT_FOUND");
}

// After power loss with sync = false, the start of a record can survive while
// the next block reads back as zeros. With nothing valid after it, that is a
// torn write -- and after recovery consolidates, a strict reopen must work too.
TEST_F(DBTest, ZeroFilledTailOfTheNewestLogIsATornWrite) {
  Open();
  ASSERT_TRUE(db_.Put("kept", "yes").ok());
  ASSERT_TRUE(db_.Put("spans-blocks", std::string(40000, 's')).ok());
  ASSERT_TRUE(db_.Close().ok());
  const auto logs = LogFiles();
  ASSERT_EQ(logs.size(), 1U);
  // Keep the first block (with the record's first fragment); zero the rest.
  std::filesystem::resize_file(logs.front(), 32768);
  std::filesystem::resize_file(logs.front(), 32768 * 2);
  Open();
  EXPECT_EQ(Get("kept"), "yes");
  EXPECT_EQ(Get("spans-blocks"), "NOT_FOUND");
  Reopen();
  EXPECT_EQ(Get("kept"), "yes") << "strict reopen after tail handling";
}

// Damage followed by valid records is not a torn write: it fails Open by
// default (ADR-0005), naming the file.
TEST_F(DBTest, DamageFollowedByValidRecordsFailsOpen) {
  Open();
  ASSERT_TRUE(db_.Put("damaged", "1").ok());
  ASSERT_TRUE(db_.Put("filler", std::string(40000, 'f')).ok());  // pushes on to block 2
  ASSERT_TRUE(db_.Put("later", "2").ok());
  ASSERT_TRUE(db_.Close().ok());
  const auto logs = LogFiles();
  ASSERT_EQ(logs.size(), 1U);
  DamageLog(logs.front(), 12);  // inside the first record
  const Status status = DB::Open(DefaultOptions(), &db_);
  EXPECT_TRUE(status.IsCorruption()) << status.ToString();
  EXPECT_NE(status.message().find(".log"), Slice::npos) << "the error must name the file";
  EXPECT_FALSE(db_.is_open());
}

// Salvage mode opens anyway, dropping the damaged block. Recovery then
// consolidates, so the damage is gone for good: a strict reopen works, and new
// writes can never collide with sequence numbers of the dropped batch.
TEST_F(DBTest, SalvagedDatabaseReopensStrictlyAndKeepsNewWrites) {
  Open();
  ASSERT_TRUE(db_.Put("damaged", "1").ok());
  ASSERT_TRUE(db_.Put("filler", std::string(40000, 'f')).ok());
  ASSERT_TRUE(db_.Put("later", "2").ok());
  ASSERT_TRUE(db_.Close().ok());
  DamageLog(LogFiles().front(), 12);

  Options salvage = DefaultOptions();
  salvage.ignore_wal_corruption = true;
  Open(salvage);
  EXPECT_EQ(Get("damaged"), "NOT_FOUND");
  EXPECT_EQ(Get("later"), "2");
  ASSERT_TRUE(db_.Put("damaged", "rewritten").ok());
  ASSERT_TRUE(db_.Close().ok());

  Open();  // strict
  EXPECT_EQ(Get("damaged"), "rewritten");
  EXPECT_EQ(Get("later"), "2");
  EXPECT_EQ(LogFiles().size(), 1U);
}

// Writes hand-built WAL files, for testing recovery rules directly.
class HandWrittenLogs : public DBTest {
 protected:
  void WriteLog(std::uint64_t number, const std::vector<WriteBatch>& batches) {
    std::filesystem::create_directories(dir_.File("db"));
    auto file =
        strata::Env::Default()->NewWritableFile(strata::LogFileName(dir_.File("db"), number));
    ASSERT_TRUE(file.ok());
    strata::log::Writer writer(file->get());
    for (const WriteBatch& batch : batches) {
      ASSERT_TRUE(writer.AddRecord(strata::WriteBatchInternal::Contents(batch)).ok());
    }
    ASSERT_TRUE((*file)->Close().ok());
  }

  static WriteBatch Batch(strata::SequenceNumber sequence, std::string_view key,
                          std::string_view value) {
    WriteBatch batch;
    batch.Put(key, value);
    strata::WriteBatchInternal::SetSequence(&batch, sequence);
    return batch;
  }
};

// A consolidation that crashed after writing its copy but before deleting the
// logs it copied leaves every batch twice. Replay must apply each once.
TEST_F(HandWrittenLogs, DuplicatesFromAnInterruptedConsolidationAreSkipped) {
  WriteLog(1, {Batch(1, "a", "1"), Batch(2, "b", "2"), Batch(3, "a", "3")});
  WriteLog(2, {Batch(1, "a", "1"), Batch(2, "b", "2"), Batch(3, "a", "3"), Batch(4, "c", "4")});
  Open();
  EXPECT_EQ(Get("a"), "3");
  EXPECT_EQ(Get("b"), "2");
  EXPECT_EQ(Get("c"), "4");
  ASSERT_TRUE(db_.Put("d", "5").ok());
  Reopen();
  EXPECT_EQ(Get("d"), "5") << "new writes must be numbered after the recovered ones";
}

// A batch that straddles already-recovered sequence numbers is not a
// duplicate; it is corruption.
TEST_F(HandWrittenLogs, PartiallyOverlappingBatchesAreCorruption) {
  WriteLog(1, {Batch(1, "a", "1"), Batch(2, "b", "2")});
  WriteBatch straddling;
  straddling.Put("x", "1");
  straddling.Put("y", "2");
  strata::WriteBatchInternal::SetSequence(&straddling, 2);  // covers 2..3
  WriteLog(2, {straddling, Batch(4, "z", "4")});
  EXPECT_TRUE(DB::Open(DefaultOptions(), &db_).IsCorruption());
}

// A thread that was already waiting on the mutex when Close ran must get an
// error, not a null log. Exercised on DBImpl directly, since DB::Close also
// drops the handle.
TEST_F(DBTest, CallsAfterCloseAreRefusedByTheEngine) {
  auto impl = strata::DBImpl::Open(DefaultOptions());
  ASSERT_TRUE(impl.ok()) << impl.status().ToString();
  ASSERT_TRUE((*impl)->Close().ok());
  WriteBatch batch;
  batch.Put("k", "v");
  EXPECT_TRUE((*impl)->Write({}, batch).IsInvalidArgument());
  EXPECT_TRUE((*impl)->Get({}, "k").status().IsInvalidArgument());
  EXPECT_TRUE((*impl)->Close().ok()) << "a second Close is a no-op";
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

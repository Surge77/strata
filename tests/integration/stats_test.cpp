// The stats API: what DB::GetStats reports after real work.

#include "strata/stats.hpp"

#include "strata/db.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "db/db_impl.hpp"
#include "support/fault_injection_env.hpp"
#include "support/file_util.hpp"
#include "support/test_dir.hpp"

namespace strata {
namespace {

using test::TestDir;

const Event* FindEvent(const std::vector<Event>& events, std::string_view name) {
  const auto found =
      std::find_if(events.begin(), events.end(), [&](const Event& e) { return e.name == name; });
  return found == events.end() ? nullptr : &*found;
}

std::string FieldValue(const Event& event, std::string_view key) {
  for (const EventField& field : event.fields) {
    if (field.key == key) {
      return field.value;
    }
  }
  return "<missing>";
}

std::string Key(int index) {
  return "key" + std::to_string(10000 + index);
}

class StatsTest : public ::testing::Test {
 protected:
  void Open(Options options = {}) {
    options.path = dir_.File("db");
    auto opened = DBImpl::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    db_ = std::move(*opened);
  }

  Stats Collect() {
    auto stats = db_->CollectStats();
    EXPECT_TRUE(stats.ok()) << stats.status().ToString();
    return stats.ok() ? *stats : Stats{};
  }

  void Put(const std::string& key, const std::string& value) {
    WriteBatch batch;
    batch.Put(key, value);
    ASSERT_TRUE(db_->Write(WriteOptions{}, batch).ok());
  }

  TestDir dir_{"stats"};
  std::unique_ptr<DBImpl> db_;
};

TEST_F(StatsTest, AFreshDatabaseReportsItsOpening) {
  Open();
  const Stats stats = Collect();
  EXPECT_EQ(stats.counters.at("get.calls"), 0U);
  EXPECT_EQ(stats.counters.at("write.calls"), 0U);
  EXPECT_EQ(stats.latencies.at("get").count, 0U);
  EXPECT_EQ(stats.levels.size(), static_cast<std::size_t>(kNumLevels));
  EXPECT_GT(stats.unix_micros, 0);
  EXPECT_TRUE(stats.background_error.empty());
  EXPECT_FALSE(stats.compaction.running);
  ASSERT_NE(FindEvent(stats.recovery_events, "db_opening"), nullptr);
  ASSERT_NE(FindEvent(stats.recovery_events, "db_opened"), nullptr);
  EXPECT_EQ(stats.recovery_events.front().name, "db_opening");
}

TEST_F(StatsTest, CountsGetsAndWritesWithTheirLatencies) {
  Open();
  for (int i = 0; i < 10; ++i) {
    Put(Key(i), "v");
  }
  for (int i = 0; i < 32; ++i) {
    static_cast<void>(db_->Get(ReadOptions{}, Key(i)));
  }
  const Stats stats = Collect();
  EXPECT_EQ(stats.counters.at("write.calls"), 10U);
  EXPECT_EQ(stats.counters.at("get.calls"), 32U);
  EXPECT_EQ(stats.counters.at("get.found"), 10U);
  EXPECT_EQ(stats.counters.at("get.memtable_answers"), 10U);
  EXPECT_EQ(stats.latencies.at("write").count, 10U);
  // One Get in 16 per thread is timed; 32 in a row on one thread hold exactly 2.
  EXPECT_EQ(stats.latencies.at("get").count, 2U);
  EXPECT_LE(stats.latencies.at("get").p50, stats.latencies.at("get").p99);
  EXPECT_GT(stats.gauges.at("memtable.bytes"), 0U);
  EXPECT_EQ(stats.gauges.at("sequence.last"), 10U);
}

TEST_F(StatsTest, RejectedWritesAndEmptyBatchesAreNotCounted) {
  Open();
  EXPECT_TRUE(db_->Write(WriteOptions{}, WriteBatch{}).ok());
  EXPECT_EQ(Collect().counters.at("write.calls"), 0U);
}

// A failed write is not an accepted one: counting it would inflate
// write.calls / write.groups, the group-commit factor.
TEST_F(StatsTest, AFailedWriteStopsTheDatabaseAndIsNotCounted) {
  test::FaultInjectionEnv env(Env::Default());
  Options options;
  options.env = &env;
  Open(options);
  env.SetSyncFailure(true, ".log");
  WriteBatch batch;
  batch.Put("a", "1");
  EXPECT_FALSE(db_->Write(WriteOptions{.sync = true}, batch).ok());
  EXPECT_FALSE(db_->Write(WriteOptions{}, batch).ok()) << "refused after the failure";
  const Stats stats = Collect();
  EXPECT_EQ(stats.counters.at("write.calls"), 0U);
  EXPECT_EQ(stats.latencies.at("write").count, 0U);
  EXPECT_FALSE(stats.background_error.empty());
  const Event* failed = FindEvent(stats.recent_events, "write_failed");
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->level, LogLevel::kError);
  env.SetSyncFailure(false);
  db_.reset();
}

TEST_F(StatsTest, RefusedGetsAreNotCounted) {
  Open();
  Put("a", "1");
  const Snapshot* snapshot = db_->GetSnapshot();
  ASSERT_TRUE(db_->ReleaseSnapshot(snapshot).ok());
  EXPECT_TRUE(db_->Get(ReadOptions{.snapshot = snapshot}, "a").status().IsInvalidArgument());
  const Stats stats = Collect();
  EXPECT_EQ(stats.counters.at("get.calls"), 0U);
  EXPECT_EQ(stats.latencies.at("get").count, 0U);
}

TEST_F(StatsTest, MemtableGetsReportANonZeroLatency) {
  Open();
  Put("a", "1");
  for (int i = 0; i < 100; ++i) {
    static_cast<void>(db_->Get(ReadOptions{}, "a"));
  }
  const LatencyStats get = Collect().latencies.at("get");
  EXPECT_GT(get.mean, 0.0) << "sub-microsecond samples were truncated to zero";
  EXPECT_GT(get.p50, 0.0);
}

// The memtable's arena is written by a synced write's leader with the DB
// mutex released; the memtable.bytes gauge must be safe to read meanwhile
// (TSan checks this).
TEST_F(StatsTest, StatsCanBePolledDuringSyncedWrites) {
  Open();
  std::atomic<bool> done{false};
  std::thread poller([&] {
    while (!done.load()) {
      static_cast<void>(db_->CollectStats());
    }
  });
  WriteBatch batch;
  for (int i = 0; i < 200; ++i) {
    batch.Clear();
    batch.Put(Key(i), std::string(512, 'v'));
    EXPECT_TRUE(db_->Write(WriteOptions{.sync = true}, batch).ok());
  }
  done = true;
  poller.join();
  EXPECT_EQ(Collect().counters.at("write.calls"), 200U);
}

TEST_F(StatsTest, SnapshotsAndIteratorsAreCounted) {
  Open();
  const Snapshot* snapshot = db_->GetSnapshot();
  { auto it = db_->NewIterator(ReadOptions{}); }
  Stats stats = Collect();
  EXPECT_EQ(stats.gauges.at("snapshots.live"), 1U);
  EXPECT_EQ(stats.counters.at("iterator.created"), 1U);
  ASSERT_TRUE(db_->ReleaseSnapshot(snapshot).ok());
  EXPECT_EQ(Collect().gauges.at("snapshots.live"), 0U);
}

TEST_F(StatsTest, AFlushShowsUpInLevelZeroCountersAndEvents) {
  Open();
  Put("a", "1");
  ASSERT_TRUE(db_->FlushMemTableForTest().ok());
  const Stats stats = Collect();
  EXPECT_EQ(stats.levels[0].files, 1U);
  EXPECT_GT(stats.levels[0].bytes, 0U);
  EXPECT_EQ(stats.counters.at("flush.count"), 1U);
  EXPECT_EQ(stats.counters.at("flush.bytes"), stats.levels[0].bytes);
  EXPECT_EQ(stats.counters.at("log.rotations"), 1U);
  const Event* flush = FindEvent(stats.recent_events, "flush_finished");
  ASSERT_NE(flush, nullptr);
  EXPECT_EQ(FieldValue(*flush, "bytes"), std::to_string(stats.levels[0].bytes));
  EXPECT_EQ(FindEvent(stats.recovery_events, "flush_finished"), nullptr)
      << "events after Open are not recovery events";
}

TEST_F(StatsTest, TableReadsHitTheCachesAndTheFilter) {
  Open();
  for (int i = 0; i < 100; i += 2) {
    Put(Key(i), "v");
  }
  ASSERT_TRUE(db_->FlushMemTableForTest().ok());
  ASSERT_TRUE(db_->Get(ReadOptions{}, Key(10)).ok());
  ASSERT_TRUE(db_->Get(ReadOptions{}, Key(10)).ok());
  // Odd keys below the table's largest key (Key(98)) fall inside its range
  // but were never written: the Bloom filter answers for them without
  // reading a block. Key(99) lies past the range and never reaches it.
  constexpr std::uint64_t kInRangeMisses = 49;
  for (int i = 1; i < 100; i += 2) {
    ASSERT_TRUE(db_->Get(ReadOptions{}, Key(i)).status().IsNotFound());
  }
  const Stats stats = Collect();
  EXPECT_EQ(stats.counters.at("get.table_reads"),
            2U + kInRangeMisses - stats.counters.at("get.filter_skips"));
  EXPECT_GT(stats.counters.at("get.filter_skips"), 40U);  // ~1% false positives
  EXPECT_GE(stats.counters.at("block_cache.hits"), 1U);
  EXPECT_GE(stats.counters.at("block_cache.misses"), 1U);
  EXPECT_GE(stats.counters.at("table_cache.hits"), 1U);
  EXPECT_EQ(stats.counters.at("table_cache.misses"), 1U);  // opened once, by the flush check
  EXPECT_GT(stats.gauges.at("block_cache.bytes"), 0U);
  EXPECT_EQ(stats.gauges.at("table_cache.tables"), 1U);
}

TEST_F(StatsTest, ACompactionIsReportedWhenItFinishes) {
  Open();
  for (int round = 0; round < 2; ++round) {
    for (int i = 0; i < 50; ++i) {
      Put(Key(i), std::to_string(round));
    }
    ASSERT_TRUE(db_->FlushMemTableForTest().ok());
  }
  ASSERT_TRUE(db_->CompactLevelForTest(0).ok());
  const Stats stats = Collect();
  EXPECT_EQ(stats.counters.at("compaction.count"), 1U);
  EXPECT_GT(stats.counters.at("compaction.bytes_written"), 0U);
  EXPECT_EQ(stats.levels[0].files, 0U);
  EXPECT_GE(stats.levels[1].files, 1U);
  EXPECT_FALSE(stats.compaction.running);
  const Event* compaction = FindEvent(stats.recent_events, "compaction_finished");
  ASSERT_NE(compaction, nullptr);
  EXPECT_EQ(FieldValue(*compaction, "level"), "0");
  EXPECT_EQ(FieldValue(*compaction, "bytes_written"),
            std::to_string(stats.counters.at("compaction.bytes_written")));
}

TEST_F(StatsTest, ReopeningRecordsTheReplayedLog) {
  Open();
  Put("a", "1");
  Put("b", "2");
  ASSERT_TRUE(db_->Close().ok());
  db_.reset();
  Open();
  const Stats stats = Collect();
  const Event* replayed = FindEvent(stats.recovery_events, "wal_replayed");
  ASSERT_NE(replayed, nullptr);
  EXPECT_EQ(FieldValue(*replayed, "batches"), "2");
  const Event* opened = FindEvent(stats.recovery_events, "db_opened");
  ASSERT_NE(opened, nullptr);
  EXPECT_EQ(FieldValue(*opened, "logs_replayed"), "1");
  EXPECT_EQ(FieldValue(*opened, "last_sequence"), "2");
}

TEST_F(StatsTest, EventsReachTheInfoLogAsLogfmtLines) {
  Open();
  ASSERT_TRUE(db_->Close().ok());
  const std::string contents = test::ReadFileToString(dir_.File("db") + "/LOG");
  EXPECT_NE(contents.find("event=db_opened "), std::string::npos) << contents;
  EXPECT_NE(contents.find("event=db_closed"), std::string::npos) << contents;
}

TEST_F(StatsTest, AClosedDatabaseRefuses) {
  Open();
  ASSERT_TRUE(db_->Close().ok());
  EXPECT_TRUE(db_->CollectStats().status().IsInvalidArgument());
}

TEST(PublicStatsTest, DbGetStatsForwardsAndRefusesWhenClosed) {
  TestDir dir("public_stats");
  DB db;
  EXPECT_TRUE(db.GetStats().status().IsInvalidArgument());
  ASSERT_TRUE(DB::Open({.path = dir.File("db")}, &db).ok());
  ASSERT_TRUE(db.Put("k", "v").ok());
  const auto stats = db.GetStats();
  ASSERT_TRUE(stats.ok()) << stats.status().ToString();
  EXPECT_EQ(stats->counters.at("write.calls"), 1U);
  const std::string json = StatsToJson(*stats);
  EXPECT_NE(json.find(R"("write.calls":1)"), std::string::npos);
}

}  // namespace
}  // namespace strata

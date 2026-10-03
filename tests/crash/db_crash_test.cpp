// Crash tests for the DB: a real SIGKILL, and simulated power loss.
//
// These are the phase-3 merge gate. They check the two halves of ADR-0005's
// promise separately:
//   * every acknowledged write survives the death of the process, whatever
//     WriteOptions::sync was;
//   * every write acknowledged with sync = true also survives power loss.

#include "strata/db.hpp"

#include <csignal>
#include <cstdint>
#include <map>
#include <random>
#include <string>

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include "support/fault_injection_env.hpp"
#include "support/test_dir.hpp"

namespace {

using strata::DB;
using strata::Env;
using strata::Options;
using strata::Status;
using strata::WriteOptions;
using strata::test::FaultInjectionEnv;
using strata::test::TestDir;

std::string Key(std::uint32_t index) {
  return "key-" + std::to_string(index);
}
std::string Value(std::uint32_t index) {
  return "value-" + std::to_string(index * 7919U);
}

// Operation i of the SIGKILL workload, a pure function of i so the parent can
// recompute exactly what the child did. Keys repeat (64 of them) and a quarter
// of operations are deletes, so a replay-order or sequence-number bug changes
// an observable value instead of passing unnoticed.
struct Op {
  std::string key;
  bool is_delete;
  std::string value;
};

Op OpAt(std::uint32_t index) {
  const std::uint32_t mixed = index * 2654435761U;
  return {"key-" + std::to_string(mixed % 64U), (mixed >> 8U) % 4U == 0,
          "v" + std::to_string(index)};
}

// The child applies operations forever, reporting each index through a pipe
// the moment the call returns Ok. It never closes the DB: it is killed.
[[noreturn]] void RunWriterChild(const std::string& path, int report_fd, std::uint32_t start) {
  DB db;
  if (!DB::Open({.path = path}, &db).ok()) {
    ::_exit(3);
  }
  for (std::uint32_t index = start;; ++index) {
    const Op op = OpAt(index);
    const Status status = op.is_delete ? db.Delete(op.key) : db.Put(op.key, op.value);
    if (!status.ok()) {
      ::_exit(4);
    }
    if (::write(report_fd, &index, sizeof(index)) != sizeof(index)) {
      ::_exit(5);
    }
  }
}

// put/delete -> kill -9 -> reopen -> get, at random moments, five times over
// one database so recovery also replays (and flushes) what earlier kills
// left. Every acknowledged operation must be reflected; the single operation
// in flight when the signal landed may or may not be.
TEST(DBCrash, SigkillNeverLosesAnAcknowledgedWrite) {
  const TestDir dir("db-sigkill");
  const std::string path = dir.File("db");
  std::mt19937 random(31337);
  std::map<std::string, std::string> model;  // state after every acknowledged op
  std::uint32_t next_index = 0;

  for (int round = 0; round < 5; ++round) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
      ::close(fds[0]);
      RunWriterChild(path, fds[1], next_index);
    }
    ::close(fds[1]);

    const std::uint32_t kill_after = 200 + static_cast<std::uint32_t>(random() % 2000);
    std::uint32_t reported = 0;
    std::uint32_t acknowledged = 0;
    while (acknowledged < kill_after &&
           ::read(fds[0], &reported, sizeof(reported)) == sizeof(reported)) {
      ++acknowledged;
    }
    ASSERT_EQ(::kill(child, SIGKILL), 0);
    while (::read(fds[0], &reported, sizeof(reported)) == sizeof(reported)) {
      ++acknowledged;  // sent before the signal landed
    }
    ::close(fds[0]);
    int wait_status = 0;
    ASSERT_EQ(::waitpid(child, &wait_status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(wait_status)) << "child exited on its own: " << wait_status;
    ASSERT_GT(acknowledged, 0U);

    // Operations next_index .. last_acked were acknowledged; last_acked + 1 was
    // possibly in flight.
    const std::uint32_t last_acked = next_index + acknowledged - 1;
    for (std::uint32_t index = next_index; index <= last_acked; ++index) {
      const Op op = OpAt(index);
      if (op.is_delete) {
        model.erase(op.key);
      } else {
        model[op.key] = op.value;
      }
    }
    const Op in_flight = OpAt(last_acked + 1);

    DB db;
    const Status opened = DB::Open({.path = path}, &db);
    ASSERT_TRUE(opened.ok()) << "round " << round << ": " << opened.ToString();
    for (std::uint32_t k = 0; k < 64; ++k) {
      const std::string key = "key-" + std::to_string(k);
      auto actual = db.Get(key);
      const std::string got = actual.ok() ? *actual : "<absent>";
      const auto expected_it = model.find(key);
      const std::string expected = expected_it == model.end() ? "<absent>" : expected_it->second;
      if (got == expected) {
        continue;
      }
      // Only the in-flight operation may explain a difference.
      const std::string if_applied = in_flight.is_delete ? "<absent>" : in_flight.value;
      ASSERT_TRUE(key == in_flight.key && got == if_applied)
          << "round " << round << ": " << key << " is " << got << ", expected " << expected;
    }
    // Replaying the in-flight operation next round makes the state
    // deterministic again (puts and deletes are idempotent).
    next_index = last_acked + 1;
  }
}

class DBPowerLoss : public ::testing::Test {
 protected:
  Options DbOptions() { return Options{.path = dir_.File("db"), .env = &env_}; }

  void Open(DB* db) {
    const Status status = DB::Open(DbOptions(), db);
    ASSERT_TRUE(status.ok()) << status.ToString();
  }

  // The process dies (nothing more reaches the disk), then the power fails.
  void CrashAndReboot(DB* db) {
    env_.SetFilesystemActive(false);
    static_cast<void>(db->Close());
    ASSERT_TRUE(env_.SimulatePowerLoss().ok());
    env_.SetFilesystemActive(true);
  }

  TestDir dir_{"db-power-loss"};
  FaultInjectionEnv env_{Env::Default()};
};

TEST_F(DBPowerLoss, SyncedWritesSurviveAcrossRepeatedOutages) {
  std::uint32_t written = 0;
  for (int outage = 0; outage < 5; ++outage) {
    DB db;
    Open(&db);
    for (std::uint32_t end = written + 100; written < end; ++written) {
      ASSERT_TRUE(db.Put(Key(written), Value(written), WriteOptions{.sync = true}).ok());
    }
    CrashAndReboot(&db);

    DB reopened;
    Open(&reopened);
    for (std::uint32_t index = 0; index < written; ++index) {
      auto value = reopened.Get(Key(index));
      ASSERT_TRUE(value.ok()) << "outage " << outage << ": synced key " << index << " lost";
      ASSERT_EQ(*value, Value(index));
    }
    ASSERT_TRUE(reopened.Close().ok());
  }
}

// Recovery must not leave a state that is not a prefix of what was
// acknowledged. Here a delete is acknowledged without sync, the process dies
// (the delete is still in the page cache), the database reopens and sees it,
// and a later synced write is followed by power loss. Replaying that old log
// from the page cache is fine only if recovery makes it durable before newer
// synced writes can depend on it; otherwise the power loss would resurrect the
// deleted key while keeping the newer write.
TEST_F(DBPowerLoss, RecoveryNeverResurrectsAWriteBehindANewerSyncedOne) {
  {
    DB db;
    Open(&db);
    ASSERT_TRUE(db.Put("k", "v", WriteOptions{.sync = true}).ok());
    ASSERT_TRUE(db.Delete("k").ok());  // acknowledged, not synced
    env_.SetFilesystemActive(false);   // the process dies; no power loss
    static_cast<void>(db.Close());
    env_.SetFilesystemActive(true);
  }
  DB reopened;
  Open(&reopened);
  ASSERT_TRUE(reopened.Get("k").status().IsNotFound()) << "the delete was replayed";
  ASSERT_TRUE(reopened.Put("j", "newer", WriteOptions{.sync = true}).ok());
  CrashAndReboot(&reopened);

  DB final;
  Open(&final);
  EXPECT_EQ(*final.Get("j"), "newer");
  EXPECT_TRUE(final.Get("k").status().IsNotFound()) << "k was resurrected under a newer write";
}

// Open flushes the recovered logs to a level-0 table, commits it through the
// MANIFEST and deletes the old logs. If the table or the commit were not
// durable before the deletions, a power loss straight after Open would lose
// the entire database.
TEST_F(DBPowerLoss, DataSurvivesPowerLossRightAfterRecovery) {
  {
    DB db;
    Open(&db);
    for (std::uint32_t index = 0; index < 100; ++index) {
      ASSERT_TRUE(db.Put(Key(index), Value(index)).ok());
    }
    ASSERT_TRUE(db.Close().ok());  // a clean close syncs the log
  }
  DB recovered;
  Open(&recovered);  // replays, flushes, deletes the old log
  CrashAndReboot(&recovered);

  DB final;
  Open(&final);
  for (std::uint32_t index = 0; index < 100; ++index) {
    auto value = final.Get(Key(index));
    ASSERT_TRUE(value.ok()) << "key " << index << " lost after recovery + power loss";
    ASSERT_EQ(*value, Value(index));
  }
}

// A clean Close makes every write durable, even ones written with
// sync = false (ADR-0005, as amended by the phase-3 review).
TEST_F(DBPowerLoss, CleanCloseMakesUnsyncedWritesDurable) {
  {
    DB db;
    Open(&db);
    ASSERT_TRUE(db.Put("unsynced", "but closed cleanly").ok());
    ASSERT_TRUE(db.Close().ok());
  }
  ASSERT_TRUE(env_.SimulatePowerLoss().ok());
  DB reopened;
  Open(&reopened);
  EXPECT_EQ(*reopened.Get("unsynced"), "but closed cleanly");
}

// The durability guarantee needs the WAL's *name* to be durable, not just its
// contents. A single synced write right after Open exercises exactly that:
// if Open did not SyncDir the new log, this key would vanish.
TEST_F(DBPowerLoss, TheFirstSyncedWriteAfterOpenSurvives) {
  DB db;
  Open(&db);
  ASSERT_TRUE(db.Put("first", "write", WriteOptions{.sync = true}).ok());
  CrashAndReboot(&db);
  DB reopened;
  Open(&reopened);
  auto value = reopened.Get("first");
  ASSERT_TRUE(value.ok()) << value.status().ToString();
  EXPECT_EQ(*value, "write");
}

// An unsynced write that follows a synced one may be lost to power loss, but
// losing it must never take the synced write with it or corrupt the log.
TEST_F(DBPowerLoss, LosingUnsyncedWritesLeavesSyncedOnesIntact) {
  DB db;
  Open(&db);
  ASSERT_TRUE(db.Put("durable", "yes", WriteOptions{.sync = true}).ok());
  for (std::uint32_t index = 0; index < 50; ++index) {
    ASSERT_TRUE(db.Put(Key(index), Value(index)).ok());
  }
  CrashAndReboot(&db);
  DB reopened;
  Open(&reopened);  // must not fail with Corruption: the lost tail is clean
  EXPECT_EQ(*reopened.Get("durable"), "yes");
}

// After a sync fails, the outcome of that write is unknown, and the log may
// hold a partial record. Every later write must fail too rather than be
// acknowledged after a gap.
TEST_F(DBPowerLoss, AFailedSyncStopsAllFurtherWrites) {
  DB db;
  Open(&db);
  ASSERT_TRUE(db.Put("before", "ok", WriteOptions{.sync = true}).ok());
  env_.SetSyncFailure(true);
  EXPECT_TRUE(db.Put("during", "x", WriteOptions{.sync = true}).IsIoError());
  env_.SetSyncFailure(false);
  EXPECT_TRUE(db.Put("after", "x").IsIoError()) << "the write path must stay stopped";
  EXPECT_TRUE(db.Get("before").ok()) << "reads keep working";
  EXPECT_TRUE(db.Close().IsIoError()) << "close must report the earlier write failure";

  DB reopened;
  Open(&reopened);
  EXPECT_EQ(*reopened.Get("before"), "ok");
  EXPECT_TRUE(reopened.Put("again", "fine").ok()) << "a fresh log after reopen accepts writes";
}

}  // namespace

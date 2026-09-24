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

// The child writes keys forever, reporting each acknowledged index through a
// pipe the moment Put returns Ok. It never closes the DB: it is killed.
[[noreturn]] void RunWriterChild(const std::string& path, int report_fd, std::uint32_t start) {
  DB db;
  if (!DB::Open({.path = path}, &db).ok()) {
    ::_exit(3);
  }
  for (std::uint32_t index = start;; ++index) {
    if (!db.Put(Key(index), Value(index)).ok()) {
      ::_exit(4);
    }
    if (::write(report_fd, &index, sizeof(index)) != sizeof(index)) {
      ::_exit(5);
    }
  }
}

// put -> kill -9 -> reopen -> get, at a random moment, several times over
// the same database so recovery also replays logs left by earlier kills.
TEST(DBCrash, SigkillNeverLosesAnAcknowledgedWrite) {
  const TestDir dir("db-sigkill");
  const std::string path = dir.File("db");
  std::mt19937 random(31337);
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

    // Let the child acknowledge a random number of writes, then kill it
    // mid-stream -- possibly in the middle of a Put.
    const std::uint32_t kill_after = 200 + static_cast<std::uint32_t>(random() % 2000);
    std::uint32_t acknowledged_up_to = next_index;
    bool any = false;
    std::uint32_t reported = 0;
    std::uint32_t seen = 0;
    while (seen < kill_after && ::read(fds[0], &reported, sizeof(reported)) == sizeof(reported)) {
      acknowledged_up_to = reported;
      any = true;
      ++seen;
    }
    ASSERT_EQ(::kill(child, SIGKILL), 0);
    // Drain acknowledgements the child sent before the signal landed.
    while (::read(fds[0], &reported, sizeof(reported)) == sizeof(reported)) {
      acknowledged_up_to = reported;
      any = true;
    }
    ::close(fds[0]);
    int wait_status = 0;
    ASSERT_EQ(::waitpid(child, &wait_status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(wait_status)) << "child exited on its own: " << wait_status;
    ASSERT_TRUE(any) << "child acknowledged nothing";

    DB db;
    const Status opened = DB::Open({.path = path}, &db);
    ASSERT_TRUE(opened.ok()) << "round " << round << ": " << opened.ToString();
    for (std::uint32_t index = 0; index <= acknowledged_up_to; ++index) {
      auto value = db.Get(Key(index));
      ASSERT_TRUE(value.ok()) << "round " << round << ": acknowledged key " << index << " lost";
      ASSERT_EQ(*value, Value(index));
    }
    next_index = acknowledged_up_to + 1;
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
  ASSERT_TRUE(db.Close().ok());

  DB reopened;
  Open(&reopened);
  EXPECT_EQ(*reopened.Get("before"), "ok");
  EXPECT_TRUE(reopened.Put("again", "fine").ok()) << "a fresh log after reopen accepts writes";
}

}  // namespace

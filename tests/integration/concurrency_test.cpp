// Phase 9: group commit and the narrowed write path.
//
// A gated Env holds a log fsync open until the test releases it, so "reads do
// not wait for a writer's fsync" and "writers queued behind an fsync are
// committed together" are checked deterministically rather than by timing.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "db/db_impl.hpp"
#include "support/test_dir.hpp"

namespace strata {
namespace {

using test::TestDir;

constexpr auto kGenerousTimeout = std::chrono::seconds(10);

// Holds every Sync of a .log file until Release(), and reports when one has
// started.
class GatedSyncEnv final : public EnvWrapper {
 public:
  GatedSyncEnv() : EnvWrapper(Env::Default()) {}

  Result<std::unique_ptr<WritableFile>> NewWritableFile(const std::string& path) override {
    STRATA_ASSIGN_OR_RETURN(auto file, target()->NewWritableFile(path));
    if (!path.ends_with(".log")) {
      return file;
    }
    return std::unique_ptr<WritableFile>(std::make_unique<GatedFile>(this, std::move(file)));
  }

  void Close() {  // gate syncs from now on
    const std::lock_guard<std::mutex> guard(mutex_);
    gated_ = true;
  }
  void Release() {
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      gated_ = false;
    }
    changed_.notify_all();
  }
  bool WaitUntilASyncIsHeld() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kGenerousTimeout, [this] { return syncs_held_ > 0; });
  }

 private:
  class GatedFile final : public WritableFile {
   public:
    GatedFile(GatedSyncEnv* env, std::unique_ptr<WritableFile> base)
        : env_(env), base_(std::move(base)) {}
    Status Append(Slice data) override { return base_->Append(data); }
    Status Flush() override { return base_->Flush(); }
    Status Sync() override {
      env_->HoldIfGated();
      return base_->Sync();
    }
    Status Close() override { return base_->Close(); }

   private:
    GatedSyncEnv* env_;
    std::unique_ptr<WritableFile> base_;
  };

  void HoldIfGated() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++syncs_held_;
    changed_.notify_all();
    changed_.wait(lock, [this] { return !gated_; });
    --syncs_held_;
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  bool gated_ = false;
  int syncs_held_ = 0;
};

class ConcurrencyTest : public ::testing::Test {
 protected:
  void Open(Env* env = nullptr) {
    auto opened = DBImpl::Open(Options{.path = dir_.File("db"), .env = env});
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    db_ = std::move(*opened);
  }
  Status Put(const std::string& key, const std::string& value, bool sync = false) {
    WriteBatch batch;
    batch.Put(key, value);
    return db_->Write(WriteOptions{.sync = sync}, batch);
  }
  std::string Get(const std::string& key) {
    const auto value = db_->Get(ReadOptions{}, key);
    if (value.status().IsNotFound()) {
      return "NOT_FOUND";
    }
    return value.ok() ? *value : value.status().ToString();
  }

  TestDir dir_{"concurrency"};
  // Declared before db_ so it outlives the database, whose Close still syncs
  // the log through it.
  GatedSyncEnv env_;
  std::unique_ptr<DBImpl> db_;
};

// Before phase 9 the writer held the database mutex through its fsync, so a
// Get had to wait for the disk. Now the fsync runs outside the mutex.
TEST_F(ConcurrencyTest, ReadsDoNotWaitForAWritersFsync) {
  Open(&env_);
  ASSERT_TRUE(Put("existing", "value").ok());
  env_.Close();
  auto writer = std::async(std::launch::async, [&] { return Put("synced", "v", true); });
  ASSERT_TRUE(env_.WaitUntilASyncIsHeld());

  auto reader = std::async(std::launch::async, [&] { return Get("existing"); });
  const bool finished = reader.wait_for(kGenerousTimeout) == std::future_status::ready;
  env_.Release();
  ASSERT_TRUE(finished) << "a Get blocked behind a writer's fsync";
  EXPECT_EQ(reader.get(), "value");
  EXPECT_TRUE(writer.get().ok());
  EXPECT_EQ(Get("synced"), "v");
}

// A write is invisible until it is acknowledged: a reader during the held
// fsync must not see it yet.
TEST_F(ConcurrencyTest, AWriteIsInvisibleUntilItsFsyncCompletes) {
  Open(&env_);
  env_.Close();
  auto writer = std::async(std::launch::async, [&] { return Put("pending", "v", true); });
  ASSERT_TRUE(env_.WaitUntilASyncIsHeld());
  EXPECT_EQ(Get("pending"), "NOT_FOUND");
  env_.Release();
  EXPECT_TRUE(writer.get().ok());
  EXPECT_EQ(Get("pending"), "v");
}

// Writers that queue while one fsync is held are committed as one group:
// one log record and one fsync for all of them.
TEST_F(ConcurrencyTest, WritersQueuedBehindAnFsyncAreCommittedAsOneGroup) {
  constexpr int kFollowers = 6;
  Open(&env_);
  env_.Close();
  auto leader = std::async(std::launch::async, [&] { return Put("leader", "v", true); });
  ASSERT_TRUE(env_.WaitUntilASyncIsHeld());

  std::vector<std::future<Status>> followers;
  for (int index = 0; index < kFollowers; ++index) {
    followers.push_back(std::async(std::launch::async, [this, index] {
      return Put("follower" + std::to_string(index), "v", true);
    }));
  }
  // Let every follower reach the queue before the leader finishes.
  const auto deadline = std::chrono::steady_clock::now() + kGenerousTimeout;
  while (db_->QueuedWritersForTest() < kFollowers && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(db_->QueuedWritersForTest(), kFollowers);
  const std::uint64_t groups_before = db_->GetStats().write_groups;
  env_.Release();
  EXPECT_TRUE(leader.get().ok());
  for (auto& follower : followers) {
    EXPECT_TRUE(follower.get().ok());
  }
  // Two groups: the leader's own (still in its fsync when counted above) and
  // one shared by all six followers. Without group commit there would be 7.
  EXPECT_EQ(db_->GetStats().write_groups - groups_before, 2U)
      << "the queued writers must share one log record and one fsync";
  for (int index = 0; index < kFollowers; ++index) {
    EXPECT_EQ(Get("follower" + std::to_string(index)), "v");
  }
}

// An unsynced leader must not take a synced follower into its group: the
// follower would be acknowledged without its fsync.
TEST_F(ConcurrencyTest, AnUnsyncedGroupNeverCarriesASyncedWrite) {
  Open(&env_);
  env_.Close();
  auto holder = std::async(std::launch::async, [&] { return Put("holder", "v", true); });
  ASSERT_TRUE(env_.WaitUntilASyncIsHeld());
  auto unsynced = std::async(std::launch::async, [&] { return Put("unsynced", "v", false); });
  while (db_->QueuedWritersForTest() < 1) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  auto synced = std::async(std::launch::async, [&] { return Put("synced", "v", true); });
  while (db_->QueuedWritersForTest() < 2) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const std::uint64_t syncs_before = db_->GetStats().log_syncs;
  env_.Release();
  EXPECT_TRUE(holder.get().ok());
  EXPECT_TRUE(unsynced.get().ok());
  EXPECT_TRUE(synced.get().ok());
  // Exactly two syncs: the holder's (counted when it completes) and the synced
  // follower's own group. Had the unsynced leader carried the synced write,
  // there would be only the holder's.
  EXPECT_EQ(db_->GetStats().log_syncs - syncs_before, 2U);
}

// Each batch is atomic even when many writers and readers race: a reader at
// a snapshot sees both keys of a batch or neither, never one alone.
TEST_F(ConcurrencyTest, BatchesStayAtomicUnderConcurrentWritersAndReaders) {
  constexpr int kWriters = 4;
  constexpr int kBatches = 300;
  Open();
  std::atomic<bool> done{false};
  std::atomic<int> torn{0};
  std::thread reader([&] {
    while (!done.load()) {
      const Snapshot* snapshot = db_->GetSnapshot();
      for (int writer = 0; writer < kWriters; ++writer) {
        const std::string suffix = std::to_string(writer);
        const auto a = db_->Get(ReadOptions{.snapshot = snapshot}, "a" + suffix);
        const auto b = db_->Get(ReadOptions{.snapshot = snapshot}, "b" + suffix);
        if (a.ok() != b.ok() || (a.ok() && *a != *b)) {
          ++torn;
        }
      }
      static_cast<void>(db_->ReleaseSnapshot(snapshot));
    }
  });
  std::vector<std::thread> writers;
  for (int writer = 0; writer < kWriters; ++writer) {
    writers.emplace_back([this, writer] {
      const std::string suffix = std::to_string(writer);
      for (int index = 0; index < kBatches; ++index) {
        WriteBatch batch;
        batch.Put("a" + suffix, std::to_string(index));
        batch.Put("b" + suffix, std::to_string(index));
        ASSERT_TRUE(db_->Write(WriteOptions{.sync = index % 10 == 0}, batch).ok());
      }
    });
  }
  for (auto& writer : writers) {
    writer.join();
  }
  done = true;
  reader.join();
  EXPECT_EQ(torn.load(), 0);
  for (int writer = 0; writer < kWriters; ++writer) {
    EXPECT_EQ(Get("a" + std::to_string(writer)), std::to_string(kBatches - 1));
  }
}

// Many threads writing while flushes and compactions run: every write lands,
// and sequence numbers stay consistent (later writes of a key win).
TEST_F(ConcurrencyTest, ManyWritersWithBackgroundWorkLoseNothing) {
  constexpr int kWriters = 8;
  constexpr int kWritesEach = 1500;
  auto opened = DBImpl::Open(Options{.path = dir_.File("db"), .write_buffer_size = 16 * 1024});
  ASSERT_TRUE(opened.ok());
  db_ = std::move(*opened);
  std::vector<std::thread> writers;
  for (int writer = 0; writer < kWriters; ++writer) {
    writers.emplace_back([this, writer] {
      for (int index = 0; index < kWritesEach; ++index) {
        const std::string key = "w" + std::to_string(writer) + "-" + std::to_string(index % 100);
        ASSERT_TRUE(Put(key, std::to_string(index), index % 50 == 0).ok());
      }
    });
  }
  for (auto& writer : writers) {
    writer.join();
  }
  db_->WaitForCompactionsForTest();
  for (int writer = 0; writer < kWriters; ++writer) {
    for (int slot = 0; slot < 100; ++slot) {
      const int last = kWritesEach - 100 + slot;
      EXPECT_EQ(Get("w" + std::to_string(writer) + "-" + std::to_string(slot)),
                std::to_string(last));
    }
  }
}

// Closing while writers are queued: every caller returns (Ok or
// InvalidArgument), nothing crashes, and the database reopens intact.
TEST_F(ConcurrencyTest, CloseWithWritersInFlightReturnsEveryCaller) {
  constexpr int kWriters = 8;
  Open();
  std::atomic<int> acknowledged{0};
  std::atomic<int> refused{0};
  std::vector<std::thread> writers;
  for (int writer = 0; writer < kWriters; ++writer) {
    writers.emplace_back([&, writer] {
      for (int index = 0;; ++index) {
        const Status status =
            Put("c" + std::to_string(writer) + "-" + std::to_string(index), "v", index % 7 == 0);
        if (status.ok()) {
          ++acknowledged;
        } else {
          EXPECT_TRUE(status.IsInvalidArgument()) << status.ToString();
          ++refused;
          return;
        }
      }
    });
  }
  while (acknowledged.load() < 500) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(db_->Close().ok());
  for (auto& writer : writers) {
    writer.join();
  }
  EXPECT_EQ(refused.load(), kWriters);
  db_ = nullptr;
  Open();
  EXPECT_EQ(Get("c0-0"), "v");
}

}  // namespace
}  // namespace strata

// Power loss while many threads write: group commit's durability promise.
//
// A synced write may be committed by another thread's group -- appended and
// fsynced by that group's leader. It is acknowledged only after that fsync, so
// every synced write acknowledged before the power fails must survive, and so
// must every write a thread made before its last acknowledged synced one.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "db/db_impl.hpp"
#include "support/fault_injection_env.hpp"
#include "support/test_dir.hpp"

namespace strata {
namespace {

using test::FaultInjectionEnv;
using test::TestDir;

constexpr int kThreads = 8;
constexpr int kRounds = 5;
constexpr int kAcknowledgedBeforeCrash = 400;

std::string Key(int thread, int index) {
  return "t" + std::to_string(thread) + "-" + std::to_string(100000 + index);
}

TEST(ConcurrentPowerLossTest, EveryAcknowledgedSyncedWriteSurvivesGroupCommit) {
  for (int round = 0; round < kRounds; ++round) {
    TestDir dir("concurrent_power_loss");
    FaultInjectionEnv env(Env::Default());
    const Options options{.path = dir.File("db"), .write_buffer_size = 32 * 1024, .env = &env};
    // Per thread: how many writes were acknowledged, and how many of those
    // were covered by an acknowledged synced write (so must be durable).
    std::vector<std::atomic<int>> acknowledged(kThreads);
    std::vector<std::atomic<int>> durable(kThreads);
    std::atomic<int> total{0};
    {
      auto opened = DBImpl::Open(options);
      ASSERT_TRUE(opened.ok()) << opened.status().ToString();
      const std::unique_ptr<DBImpl> db = std::move(*opened);
      std::vector<std::thread> writers;
      for (int thread = 0; thread < kThreads; ++thread) {
        writers.emplace_back([&, thread] {
          for (int index = 0;; ++index) {
            const bool sync = index % 3 == 0;
            WriteBatch batch;
            batch.Put(Key(thread, index), std::to_string(index));
            if (!db->Write(WriteOptions{.sync = sync}, batch).ok()) {
              return;  // the crash: nothing more reaches the disk
            }
            acknowledged[static_cast<std::size_t>(thread)] = index + 1;
            if (sync) {
              durable[static_cast<std::size_t>(thread)] = index + 1;
            }
            ++total;
          }
        });
      }
      while (total.load() < kAcknowledgedBeforeCrash) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      env.SetFilesystemActive(false);  // the process dies mid-stream
      for (auto& writer : writers) {
        writer.join();
      }
      static_cast<void>(db->Close());
    }
    ASSERT_TRUE(env.SimulatePowerLoss().ok());
    env.SetFilesystemActive(true);

    auto reopened = DBImpl::Open(options);
    ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
    const std::unique_ptr<DBImpl> db = std::move(*reopened);
    for (int thread = 0; thread < kThreads; ++thread) {
      const int must_survive = durable[static_cast<std::size_t>(thread)].load();
      for (int index = 0; index < must_survive; ++index) {
        const auto value = db->Get(ReadOptions{}, Key(thread, index));
        ASSERT_TRUE(value.ok()) << "round " << round << ": thread " << thread << " write " << index
                                << " was covered by an acknowledged sync but lost";
        EXPECT_EQ(*value, std::to_string(index));
      }
      // And no thread's writes have a gap: what survived is a prefix.
      int present = must_survive;
      while (present < acknowledged[static_cast<std::size_t>(thread)].load() + 1 &&
             db->Get(ReadOptions{}, Key(thread, present)).ok()) {
        ++present;
      }
      for (int index = present; index <= acknowledged[static_cast<std::size_t>(thread)].load();
           ++index) {
        EXPECT_TRUE(db->Get(ReadOptions{}, Key(thread, index)).status().IsNotFound())
            << "round " << round << ": thread " << thread << " has a gap before write " << index;
      }
    }
  }
}

}  // namespace
}  // namespace strata

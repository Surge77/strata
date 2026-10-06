// Power loss in the window between a log rotation and the flush of the
// memtable it retired.
//
// When the memtable fills, writes move to a new log while the old memtable is
// flushed in the background. Until that flush commits, the data lives only in
// the two logs. A synced write to the new log in that window promises two
// things: that write survives, and so does every write acknowledged before
// it, which is in the old log. The sync-removal negative control found that
// no test exercised this window deterministically.

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

// Well above the memtable's own overhead (its first arena block), so the
// fresh memtable after the rotation has room for the synced write. Otherwise
// that write would need a second rotation and wait for the paused flush.
constexpr std::size_t kWriteBuffer = 64 * 1024;
constexpr int kMaxWritesBeforeRotation = 1000;

std::string Key(int index) {
  return "key" + std::to_string(1000 + index);
}
std::string Value(int index) {
  return std::string(100, static_cast<char>('a' + index % 26)) + std::to_string(index);
}

TEST(RotationPowerLossTest, SyncedWriteAfterRotationKeepsItselfAndEverythingBefore) {
  TestDir dir("rotation_power_loss");
  FaultInjectionEnv env(Env::Default());
  const Options options{.path = dir.File("db"), .write_buffer_size = kWriteBuffer, .env = &env};
  int written = 0;
  {
    auto opened = DBImpl::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    const std::unique_ptr<DBImpl> db = std::move(*opened);
    db->PauseFlushesForTest(true);  // hold the retired memtable in the window

    // Unsynced writes until the memtable is retired to a new log.
    while (!db->HasImmutableMemTableForTest()) {
      ASSERT_LT(written, kMaxWritesBeforeRotation) << "the memtable never rotated";
      WriteBatch batch;
      batch.Put(Key(written), Value(written));
      ASSERT_TRUE(db->Write(WriteOptions{}, batch).ok());
      ++written;
    }
    // The synced write lands in the new log; the old memtable is unflushed.
    WriteBatch synced;
    synced.Put("synced", "after rotation");
    ASSERT_TRUE(db->Write(WriteOptions{.sync = true}, synced).ok());

    env.SetFilesystemActive(false);  // the process dies here
    static_cast<void>(db->Close());
  }
  ASSERT_TRUE(env.SimulatePowerLoss().ok());
  env.SetFilesystemActive(true);

  auto reopened = DBImpl::Open(options);
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  const std::unique_ptr<DBImpl> db = std::move(*reopened);
  const auto synced = db->Get(ReadOptions{}, "synced");
  ASSERT_TRUE(synced.ok()) << "the synced write was lost: " << synced.status().ToString();
  for (int index = 0; index < written; ++index) {
    const auto value = db->Get(ReadOptions{}, Key(index));
    ASSERT_TRUE(value.ok()) << "write " << index << " before the synced one was lost";
    EXPECT_EQ(*value, Value(index));
  }
}

}  // namespace
}  // namespace strata

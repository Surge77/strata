#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "db/db_impl.hpp"
#include "support/fault_injection_env.hpp"
#include "support/test_dir.hpp"

namespace {

using strata::DBImpl;
using strata::Env;
using strata::Options;
using strata::ReadOptions;
using strata::WriteBatch;
using strata::WriteOptions;
using strata::test::FaultInjectionEnv;
using strata::test::TestDir;

// Tiny memtables so the workload flushes several times, and the crash points
// cover memtable rotation, table building, MANIFEST commits, CURRENT
// publication and file GC -- not just log appends.
constexpr std::size_t kWriteBuffer = 8 * 1024;
constexpr std::uint32_t kWrites = 150;
constexpr std::uint32_t kSyncEvery = 10;

std::string Key(std::uint32_t index) {
  return "key-" + std::to_string(index);
}
std::string Value(std::uint32_t index) {
  return std::string(100, static_cast<char>('a' + index % 26)) + std::to_string(index);
}

struct Outcome {
  // Highest index whose synced write -- and so every write before it -- was
  // acknowledged; nullopt if none was.
  std::optional<std::uint32_t> last_durable;
};

// Runs the workload until it finishes or the injected crash stops it.
Outcome RunWorkload(const std::string& path, Env* env) {
  Options options;
  options.path = path;
  options.env = env;
  options.write_buffer_size = kWriteBuffer;
  Outcome outcome;
  auto opened = DBImpl::Open(options);
  if (!opened.ok()) {
    return outcome;
  }
  std::unique_ptr<DBImpl> db = std::move(*opened);
  for (std::uint32_t index = 0; index < kWrites; ++index) {
    WriteBatch batch;
    batch.Put(Key(index), Value(index));
    const bool sync = (index + 1) % kSyncEvery == 0;
    if (!db->Write(WriteOptions{.sync = sync}, batch).ok()) {
      break;
    }
    if (sync) {
      outcome.last_durable = index;
    }
    // Keeps the op sequence deterministic, so crash point k is the same
    // moment in every run.
    db->WaitForFlush();
  }
  static_cast<void>(db->Close());
  return outcome;
}

TEST(FlushCrashTest, EveryCrashPointRecoversAnAcknowledgedPrefix) {
  std::uint64_t total_ops = 0;
  {
    TestDir dir("flush_crash_baseline");
    FaultInjectionEnv env(Env::Default());
    const std::uint64_t before = env.mutating_ops();
    RunWorkload(dir.File("db"), &env);
    total_ops = env.mutating_ops() - before;
  }
  ASSERT_GT(total_ops, 0U);

  for (std::uint64_t crash_at = 0; crash_at <= total_ops; ++crash_at) {
    TestDir dir("flush_crash");
    FaultInjectionEnv env(Env::Default());
    env.SetCrashAfterOps(crash_at);
    const Outcome outcome = RunWorkload(dir.File("db"), &env);

    env.SetCrashAfterOps(std::nullopt);
    ASSERT_TRUE(env.SimulatePowerLoss().ok());
    env.SetFilesystemActive(true);

    Options options;
    options.path = dir.File("db");
    options.env = &env;
    options.write_buffer_size = kWriteBuffer;
    auto reopened = DBImpl::Open(options);
    ASSERT_TRUE(reopened.ok()) << "crash at op " << crash_at << ": "
                               << reopened.status().ToString();
    const std::unique_ptr<DBImpl> db = std::move(*reopened);

    // The recovered writes must be exactly indexes [0, present): a prefix of
    // what was attempted, reaching at least the last acknowledged sync.
    std::uint32_t present = 0;
    for (std::uint32_t index = 0; index < kWrites; ++index) {
      const auto value = db->Get(ReadOptions{}, Key(index));
      if (value.status().IsNotFound()) {
        break;
      }
      ASSERT_TRUE(value.ok()) << "crash at op " << crash_at << ": " << value.status().ToString();
      ASSERT_EQ(*value, Value(index)) << "crash at op " << crash_at;
      present = index + 1;
    }
    for (std::uint32_t index = present; index < kWrites; ++index) {
      ASSERT_TRUE(db->Get(ReadOptions{}, Key(index)).status().IsNotFound())
          << "crash at op " << crash_at << ": key " << index << " present after a gap";
    }
    if (outcome.last_durable) {
      ASSERT_GT(present, *outcome.last_durable)
          << "crash at op " << crash_at << ": synced write " << *outcome.last_durable << " lost";
    }
    ASSERT_TRUE(db->Close().ok());
  }
}

}  // namespace

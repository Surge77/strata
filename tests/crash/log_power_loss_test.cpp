// Power-loss tests for the log: real files on ext4, through FaultInjectionEnv.
//
// The claim under test is the one the WAL will rely on in phase 3: every record
// written before a successful Sync survives power loss intact, and whatever
// was written after it disappears cleanly -- as a shorter log, never as a
// corrupt one.

#include <memory>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "log/log_reader.hpp"
#include "log/log_writer.hpp"
#include "support/fault_injection_env.hpp"
#include "support/test_dir.hpp"

namespace {

using strata::Env;
using strata::Slice;
using strata::Status;
using strata::log::Reader;
using strata::log::Writer;
using strata::test::FaultInjectionEnv;
using strata::test::TestDir;

class FailOnReport final : public Reader::Reporter {
 public:
  void Corruption(std::size_t bytes_dropped, const Status& reason) override {
    ADD_FAILURE() << "reader reported " << bytes_dropped << " bytes: " << reason.ToString();
  }
};

std::vector<std::string> ReadAll(Env* env, const std::string& path) {
  auto file = env->NewSequentialFile(path);
  EXPECT_TRUE(file.ok()) << file.status().ToString();
  if (!file.ok()) {
    return {};
  }
  FailOnReport reporter;
  Reader reader(file->get(), &reporter);
  std::vector<std::string> records;
  Slice record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
    records.emplace_back(record);
  }
  return records;
}

std::string Payload(std::mt19937& random, std::size_t index) {
  // Mostly small, sometimes spanning blocks, so power loss can land inside any
  // fragment type.
  const std::size_t length = random() % 8 == 0 ? 40000 + random() % 30000 : random() % 400;
  std::string payload(length, '\0');
  for (std::size_t offset = 0; offset < length; ++offset) {
    payload[offset] = static_cast<char>((index + offset * 13U) & 0xFFU);
  }
  return payload;
}

// Kills the "process" and cuts the power, in that order, like a real outage:
// first nothing more reaches the disk, then everything unsynced is lost.
void CrashAndReboot(FaultInjectionEnv& env, std::unique_ptr<strata::WritableFile>& file) {
  env.SetFilesystemActive(false);
  file.reset();
  ASSERT_TRUE(env.SimulatePowerLoss().ok());
  env.SetFilesystemActive(true);
}

TEST(LogPowerLoss, SyncedRecordsSurviveAndUnsyncedOnesVanishCleanly) {
  const TestDir dir("log-power-loss");
  FaultInjectionEnv env(Env::Default());
  const std::string path = dir.File("000001.log");

  auto file = env.NewWritableFile(path);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(env.SyncDir(dir.path()).ok());  // the log's name must be durable too
  std::unique_ptr<strata::WritableFile> handle = std::move(file).value();
  Writer writer(handle.get());

  std::mt19937 random(2024);
  std::vector<std::string> synced;
  for (std::size_t index = 0; index < 50; ++index) {
    synced.push_back(Payload(random, index));
    ASSERT_TRUE(writer.AddRecord(synced.back()).ok());
  }
  ASSERT_TRUE(handle->Sync().ok());
  for (std::size_t index = 50; index < 80; ++index) {
    ASSERT_TRUE(writer.AddRecord(Payload(random, index)).ok());  // flushed, never synced
  }

  CrashAndReboot(env, handle);
  EXPECT_EQ(ReadAll(Env::Default(), path), synced);
}

// Many independent outages, each with a random amount of synced and unsynced
// writing before it; the recovered log must always equal the synced prefix.
TEST(LogPowerLoss, RandomSyncPointsAlwaysRecoverExactlyTheSyncedPrefix) {
  std::mt19937 random(99);
  for (int round = 0; round < 25; ++round) {
    const TestDir dir("log-power-loss-random");
    FaultInjectionEnv env(Env::Default());
    const std::string path = dir.File("000001.log");
    auto file = env.NewWritableFile(path);
    ASSERT_TRUE(file.ok());
    ASSERT_TRUE(env.SyncDir(dir.path()).ok());
    std::unique_ptr<strata::WritableFile> handle = std::move(file).value();
    Writer writer(handle.get());

    std::vector<std::string> written;
    std::size_t synced_count = 0;
    const int operations = 20 + static_cast<int>(random() % 60);
    for (int op = 0; op < operations; ++op) {
      written.push_back(Payload(random, written.size()));
      ASSERT_TRUE(writer.AddRecord(written.back()).ok());
      if (random() % 5 == 0) {
        ASSERT_TRUE(handle->Sync().ok());
        synced_count = written.size();
      }
    }

    CrashAndReboot(env, handle);
    const std::vector<std::string> expected(written.begin(),
                                            written.begin() + static_cast<long>(synced_count));
    ASSERT_EQ(ReadAll(Env::Default(), path), expected) << "round " << round;
  }
}

// Why the WAL must SyncDir after creating a log: with every record synced but
// the directory entry not, power loss takes the whole file with it.
TEST(LogPowerLoss, SyncingRecordsIsNotEnoughWithoutSyncingTheDirectory) {
  const TestDir dir("log-power-loss-dir");
  FaultInjectionEnv env(Env::Default());
  const std::string path = dir.File("000001.log");
  auto file = env.NewWritableFile(path);
  ASSERT_TRUE(file.ok());
  std::unique_ptr<strata::WritableFile> handle = std::move(file).value();
  Writer writer(handle.get());
  ASSERT_TRUE(writer.AddRecord("acknowledged").ok());
  ASSERT_TRUE(handle->Sync().ok());

  CrashAndReboot(env, handle);
  EXPECT_FALSE(Env::Default()->FileExists(path));
}

}  // namespace

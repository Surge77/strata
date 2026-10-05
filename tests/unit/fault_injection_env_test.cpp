#include "support/fault_injection_env.hpp"

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "support/test_dir.hpp"

namespace {

using strata::Env;
using strata::Slice;
using strata::test::FaultInjectionEnv;
using strata::test::TestDir;

std::string ReadAll(Env* env, const std::string& path) {
  auto file = env->NewSequentialFile(path);
  if (!file.ok()) {
    return "<missing>";
  }
  std::string contents;
  char scratch[4096];
  while (true) {
    auto chunk = (*file)->Read(sizeof(scratch), scratch);
    if (!chunk.ok() || chunk->empty()) {
      break;
    }
    contents.append(*chunk);
  }
  return contents;
}

void WriteDurably(Env* env, const std::string& path, Slice contents) {
  auto file = env->NewWritableFile(path);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE((*file)->Append(contents).ok());
  ASSERT_TRUE((*file)->Sync().ok());
  ASSERT_TRUE((*file)->Close().ok());
}

class FaultInjectionEnvTest : public ::testing::Test {
 protected:
  TestDir dir_{"fault-env"};
  FaultInjectionEnv env_{Env::Default()};
};

TEST_F(FaultInjectionEnvTest, UnsyncedAppendsAreLostAndSyncedOnesKept) {
  const std::string path = dir_.File("f");
  ASSERT_TRUE(env_.SyncDir(dir_.path()).ok());
  auto file = env_.NewWritableFile(path);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(env_.SyncDir(dir_.path()).ok());  // make the file's name durable
  ASSERT_TRUE((*file)->Append("synced-").ok());
  ASSERT_TRUE((*file)->Sync().ok());
  ASSERT_TRUE((*file)->Append("lost").ok());
  ASSERT_TRUE((*file)->Flush().ok());  // in the OS, but not on disk
  ASSERT_TRUE((*file)->Close().ok());
  EXPECT_EQ(ReadAll(&env_, path), "synced-lost");

  ASSERT_TRUE(env_.SimulatePowerLoss().ok());
  EXPECT_EQ(ReadAll(&env_, path), "synced-");
}

TEST_F(FaultInjectionEnvTest, NewFileVanishesWithoutSyncDir) {
  const std::string path = dir_.File("new");
  WriteDurably(&env_, path, "contents synced, name not");
  ASSERT_TRUE(env_.SimulatePowerLoss().ok());
  EXPECT_FALSE(env_.FileExists(path));
}

TEST_F(FaultInjectionEnvTest, NewFileSurvivesAfterSyncDir) {
  const std::string path = dir_.File("new");
  WriteDurably(&env_, path, "fully durable");
  ASSERT_TRUE(env_.SyncDir(dir_.path()).ok());
  ASSERT_TRUE(env_.SimulatePowerLoss().ok());
  EXPECT_EQ(ReadAll(&env_, path), "fully durable");
}

// The manifest publish pattern: write a temp file, sync it, rename it over the
// live name. Without SyncDir the old live file must come back.
TEST_F(FaultInjectionEnvTest, RenameOverTargetIsUndoneWithoutSyncDir) {
  const std::string live = dir_.File("CURRENT");
  const std::string temp = dir_.File("CURRENT.tmp");
  WriteDurably(&env_, live, "old");
  ASSERT_TRUE(env_.SyncDir(dir_.path()).ok());

  WriteDurably(&env_, temp, "new");
  ASSERT_TRUE(env_.RenameFile(temp, live).ok());
  EXPECT_EQ(ReadAll(&env_, live), "new");

  ASSERT_TRUE(env_.SimulatePowerLoss().ok());
  EXPECT_EQ(ReadAll(&env_, live), "old");
  EXPECT_FALSE(env_.FileExists(temp)) << "temp file was never made durable either";
}

TEST_F(FaultInjectionEnvTest, RenameSurvivesAfterSyncDir) {
  const std::string live = dir_.File("CURRENT");
  const std::string temp = dir_.File("CURRENT.tmp");
  WriteDurably(&env_, live, "old");
  WriteDurably(&env_, temp, "new");
  ASSERT_TRUE(env_.RenameFile(temp, live).ok());
  ASSERT_TRUE(env_.SyncDir(dir_.path()).ok());
  ASSERT_TRUE(env_.SimulatePowerLoss().ok());
  EXPECT_EQ(ReadAll(&env_, live), "new");
  EXPECT_FALSE(env_.FileExists(temp));
}

TEST_F(FaultInjectionEnvTest, RemovalIsUndoneWithoutSyncDir) {
  const std::string path = dir_.File("victim");
  WriteDurably(&env_, path, "still here");
  ASSERT_TRUE(env_.SyncDir(dir_.path()).ok());
  ASSERT_TRUE(env_.RemoveFile(path).ok());
  EXPECT_FALSE(env_.FileExists(path));
  ASSERT_TRUE(env_.SimulatePowerLoss().ok());
  EXPECT_EQ(ReadAll(&env_, path), "still here");
}

TEST_F(FaultInjectionEnvTest, PreExistingFilesAreDurable) {
  const std::string path = dir_.File("old");
  {
    auto file = Env::Default()->NewWritableFile(path);  // written outside the fault env
    ASSERT_TRUE(file.ok());
    ASSERT_TRUE((*file)->Append("before").ok());
    ASSERT_TRUE((*file)->Close().ok());
  }
  auto appender = env_.NewAppendableFile(path);
  ASSERT_TRUE(appender.ok());
  ASSERT_TRUE((*appender)->Append("+after").ok());
  ASSERT_TRUE((*appender)->Close().ok());
  ASSERT_TRUE(env_.SimulatePowerLoss().ok());
  EXPECT_EQ(ReadAll(&env_, path), "before");
}

TEST_F(FaultInjectionEnvTest, InactiveFilesystemRejectsEveryWrite) {
  const std::string path = dir_.File("f");
  auto file = env_.NewWritableFile(path);
  ASSERT_TRUE(file.ok());
  env_.SetFilesystemActive(false);
  EXPECT_TRUE((*file)->Append("x").IsIoError());
  EXPECT_TRUE((*file)->Flush().IsIoError());
  EXPECT_TRUE((*file)->Sync().IsIoError());
  EXPECT_TRUE(env_.NewWritableFile(dir_.File("g")).status().IsIoError());
  EXPECT_TRUE(env_.NewAppendableFile(dir_.File("g")).status().IsIoError());
  EXPECT_TRUE(env_.RemoveFile(path).IsIoError());
  EXPECT_TRUE(env_.RenameFile(path, dir_.File("h")).IsIoError());
  EXPECT_TRUE(env_.SyncDir(dir_.path()).IsIoError());
  env_.SetFilesystemActive(true);
  EXPECT_TRUE((*file)->Append("x").ok());
}

TEST_F(FaultInjectionEnvTest, InjectedSyncFailuresHaveNoDurableEffect) {
  const std::string path = dir_.File("f");
  ASSERT_TRUE(env_.SyncDir(dir_.path()).ok());
  auto file = env_.NewWritableFile(path);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(env_.SyncDir(dir_.path()).ok());
  ASSERT_TRUE((*file)->Append("data").ok());
  env_.SetSyncFailure(true);
  EXPECT_TRUE((*file)->Sync().IsIoError());
  EXPECT_TRUE(env_.SyncDir(dir_.path()).IsIoError());
  env_.SetSyncFailure(false);
  ASSERT_TRUE((*file)->Close().ok());
  ASSERT_TRUE(env_.SimulatePowerLoss().ok());
  EXPECT_EQ(ReadAll(&env_, path), "");
}

TEST_F(FaultInjectionEnvTest, CountsSuccessfulSyncs) {
  EXPECT_EQ(env_.sync_count(), 0U);
  WriteDurably(&env_, dir_.File("f"), "x");
  ASSERT_TRUE(env_.SyncDir(dir_.path()).ok());
  EXPECT_EQ(env_.sync_count(), 1U);
  EXPECT_EQ(env_.sync_dir_count(), 1U);
}

TEST_F(FaultInjectionEnvTest, TruncatingAnExistingFileLosesOldContentsUntilSynced) {
  const std::string path = dir_.File("f");
  WriteDurably(&env_, path, "original");
  ASSERT_TRUE(env_.SyncDir(dir_.path()).ok());
  auto file = env_.NewWritableFile(path);  // O_TRUNC
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE((*file)->Append("replacement").ok());
  ASSERT_TRUE((*file)->Close().ok());
  ASSERT_TRUE(env_.SimulatePowerLoss().ok());
  EXPECT_EQ(ReadAll(&env_, path), "");
}

TEST_F(FaultInjectionEnvTest, DiskFullWritesWhatFitsThenFails) {
  const std::string path = dir_.File("full.sst");
  env_.SetDiskFull(".sst", 4);
  auto file = env_.NewWritableFile(path);
  ASSERT_TRUE(file.ok());
  const strata::Status status = (*file)->Append("abcdefgh");
  EXPECT_TRUE(status.IsIoError());
  EXPECT_NE(status.ToString().find("No space left"), std::string::npos) << status.ToString();
  EXPECT_TRUE((*file)->Append("x").IsIoError()) << "the disk stays full";
  ASSERT_TRUE((*file)->Close().ok());
  EXPECT_EQ(ReadAll(&env_, path), "abcd") << "a real ENOSPC can leave a partial write";
}

TEST_F(FaultInjectionEnvTest, DiskFullOnlyAffectsMatchingFiles) {
  env_.SetDiskFull(".sst", 0);
  auto log = env_.NewWritableFile(dir_.File("000001.log"));
  ASSERT_TRUE(log.ok());
  EXPECT_TRUE((*log)->Append("still fine").ok());
  env_.ClearDiskFull();
  auto table = env_.NewWritableFile(dir_.File("000002.sst"));
  ASSERT_TRUE(table.ok());
  EXPECT_TRUE((*table)->Append("space again").ok());
}

TEST_F(FaultInjectionEnvTest, ReadFailureHitsOnlyMatchingFiles) {
  WriteDurably(&env_, dir_.File("a.sst"), "table bytes");
  WriteDurably(&env_, dir_.File("b.log"), "log bytes");
  env_.SetReadFailure(".sst");
  auto table = env_.NewRandomAccessFile(dir_.File("a.sst"));
  ASSERT_TRUE(table.ok());
  char scratch[16];
  const auto failed = (*table)->Read(0, 5, scratch);
  EXPECT_TRUE(failed.status().IsIoError());
  auto log = env_.NewRandomAccessFile(dir_.File("b.log"));
  ASSERT_TRUE(log.ok());
  const auto read = (*log)->Read(0, 3, scratch);
  ASSERT_TRUE(read.ok());
  EXPECT_EQ(strata::ToString(*read), "log");

  env_.SetReadFailure(std::nullopt);
  const auto recovered = (*table)->Read(0, 5, scratch);
  ASSERT_TRUE(recovered.ok()) << "failure is checked per read, not per open";
  EXPECT_EQ(strata::ToString(*recovered), "table");
}

}  // namespace

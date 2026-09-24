#include "strata/env.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include "support/test_dir.hpp"

namespace {

using strata::Env;
using strata::FileLock;
using strata::Slice;
using strata::Status;
using strata::test::TestDir;

Env* env() {
  return Env::Default();
}

void WriteFile(const std::string& path, Slice contents) {
  auto file = env()->NewWritableFile(path);
  ASSERT_TRUE(file.ok()) << file.status().ToString();
  ASSERT_TRUE((*file)->Append(contents).ok());
  ASSERT_TRUE((*file)->Close().ok());
}

std::string ReadWholeFile(const std::string& path) {
  auto file = env()->NewSequentialFile(path);
  EXPECT_TRUE(file.ok()) << file.status().ToString();
  if (!file.ok()) {
    return {};
  }
  std::string contents;
  std::vector<char> scratch(1000);
  while (true) {
    auto chunk = (*file)->Read(scratch.size(), scratch.data());
    EXPECT_TRUE(chunk.ok());
    if (!chunk.ok() || chunk->empty()) {
      break;
    }
    contents.append(*chunk);
  }
  return contents;
}

// A string whose bytes differ at every position, so an off-by-one in a read
// loop shows up as a content mismatch rather than going unnoticed.
std::string PatternBytes(std::size_t size) {
  std::string bytes(size, '\0');
  for (std::size_t index = 0; index < size; ++index) {
    bytes[index] = static_cast<char>((index * 7U + index / 251U) & 0xFFU);
  }
  return bytes;
}

TEST(PosixEnv, DefaultIsASingleInstance) {
  EXPECT_EQ(Env::Default(), Env::Default());
}

TEST(PosixEnv, WritesAndReadsBackSmallFile) {
  const TestDir dir("env-small");
  WriteFile(dir.File("f"), "hello, world");
  EXPECT_EQ(ReadWholeFile(dir.File("f")), "hello, world");
}

// Larger than the writer's 64 KiB buffer, with appends both smaller and larger
// than it, so every branch of Append's buffering is taken.
TEST(PosixEnv, WritesAndReadsBackAcrossBufferBoundaries) {
  const TestDir dir("env-large");
  const std::string expected = PatternBytes(300 * 1024);
  {
    auto file = env()->NewWritableFile(dir.File("f"));
    ASSERT_TRUE(file.ok());
    const Slice all(expected);
    ASSERT_TRUE((*file)->Append(all.substr(0, 100)).ok());
    ASSERT_TRUE((*file)->Append(all.substr(100, 70 * 1024)).ok());
    ASSERT_TRUE((*file)->Append(all.substr(100 + 70 * 1024, 10)).ok());
    ASSERT_TRUE((*file)->Append(all.substr(110 + 70 * 1024)).ok());
    ASSERT_TRUE((*file)->Sync().ok());
    ASSERT_TRUE((*file)->Close().ok());
  }
  EXPECT_EQ(ReadWholeFile(dir.File("f")), expected);
}

TEST(PosixEnv, FlushMakesBytesVisibleToOtherReaders) {
  const TestDir dir("env-flush");
  auto file = env()->NewWritableFile(dir.File("f"));
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE((*file)->Append("abc").ok());
  ASSERT_TRUE((*file)->Flush().ok());
  EXPECT_EQ(ReadWholeFile(dir.File("f")), "abc");
  ASSERT_TRUE((*file)->Close().ok());
}

TEST(PosixEnv, DestroyingAnUnclosedFileStillWritesItsBuffer) {
  const TestDir dir("env-dtor");
  {
    auto file = env()->NewWritableFile(dir.File("f"));
    ASSERT_TRUE(file.ok());
    ASSERT_TRUE((*file)->Append("buffered").ok());
  }
  EXPECT_EQ(ReadWholeFile(dir.File("f")), "buffered");
}

TEST(PosixEnv, SequentialReadReportsEndOfFileAsShortRead) {
  const TestDir dir("env-eof");
  WriteFile(dir.File("f"), "abcdef");
  auto file = env()->NewSequentialFile(dir.File("f"));
  ASSERT_TRUE(file.ok());
  char scratch[16];
  auto first = (*file)->Read(4, scratch);
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(*first, "abcd");
  auto second = (*file)->Read(4, scratch);
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(*second, "ef");
  auto third = (*file)->Read(4, scratch);
  ASSERT_TRUE(third.ok());
  EXPECT_TRUE(third->empty());
}

TEST(PosixEnv, SequentialSkipAdvances) {
  const TestDir dir("env-skip");
  WriteFile(dir.File("f"), "0123456789");
  auto file = env()->NewSequentialFile(dir.File("f"));
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE((*file)->Skip(7).ok());
  char scratch[8];
  auto rest = (*file)->Read(8, scratch);
  ASSERT_TRUE(rest.ok());
  EXPECT_EQ(*rest, "789");
}

TEST(PosixEnv, RandomAccessReadsAtOffsets) {
  const TestDir dir("env-random");
  WriteFile(dir.File("f"), "0123456789");
  auto file = env()->NewRandomAccessFile(dir.File("f"));
  ASSERT_TRUE(file.ok());
  char scratch[8];
  auto middle = (*file)->Read(3, 4, scratch);
  ASSERT_TRUE(middle.ok());
  EXPECT_EQ(*middle, "3456");
  auto tail = (*file)->Read(8, 8, scratch);
  ASSERT_TRUE(tail.ok());
  EXPECT_EQ(*tail, "89");
  auto past_end = (*file)->Read(20, 4, scratch);
  ASSERT_TRUE(past_end.ok());
  EXPECT_TRUE(past_end->empty());
}

TEST(PosixEnv, NewWritableFileTruncates) {
  const TestDir dir("env-trunc");
  WriteFile(dir.File("f"), "long original contents");
  WriteFile(dir.File("f"), "new");
  EXPECT_EQ(ReadWholeFile(dir.File("f")), "new");
}

TEST(PosixEnv, AppendableFileKeepsExistingContents) {
  const TestDir dir("env-append");
  WriteFile(dir.File("f"), "first,");
  auto file = env()->NewAppendableFile(dir.File("f"));
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE((*file)->Append("second").ok());
  ASSERT_TRUE((*file)->Close().ok());
  EXPECT_EQ(ReadWholeFile(dir.File("f")), "first,second");
}

TEST(PosixEnv, MissingFilesAreNotFound) {
  const TestDir dir("env-missing");
  const std::string missing = dir.File("nope");
  EXPECT_FALSE(env()->FileExists(missing));
  EXPECT_TRUE(env()->NewSequentialFile(missing).status().IsNotFound());
  EXPECT_TRUE(env()->NewRandomAccessFile(missing).status().IsNotFound());
  EXPECT_TRUE(env()->GetFileSize(missing).status().IsNotFound());
  EXPECT_TRUE(env()->RemoveFile(missing).IsNotFound());
}

TEST(PosixEnv, ErrorMessagesNameThePath) {
  const TestDir dir("env-message");
  const Status status = env()->NewSequentialFile(dir.File("absent")).status();
  EXPECT_NE(status.message().find("absent"), Slice::npos) << status.ToString();
}

TEST(PosixEnv, ReportsFileSize) {
  const TestDir dir("env-size");
  WriteFile(dir.File("f"), PatternBytes(12345));
  auto size = env()->GetFileSize(dir.File("f"));
  ASSERT_TRUE(size.ok());
  EXPECT_EQ(*size, 12345U);
}

TEST(PosixEnv, ListsChildrenWithoutDotEntries) {
  const TestDir dir("env-children");
  WriteFile(dir.File("a"), "1");
  WriteFile(dir.File("b"), "2");
  ASSERT_TRUE(env()->CreateDir(dir.File("sub")).ok());
  auto children = env()->GetChildren(dir.path());
  ASSERT_TRUE(children.ok());
  std::vector<std::string> names = *children;
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names, (std::vector<std::string>{"a", "b", "sub"}));
  EXPECT_TRUE(env()->GetChildren(dir.File("missing")).status().IsNotFound());
}

TEST(PosixEnv, RenameReplacesTheTarget) {
  const TestDir dir("env-rename");
  WriteFile(dir.File("new"), "fresh");
  WriteFile(dir.File("current"), "stale");
  ASSERT_TRUE(env()->RenameFile(dir.File("new"), dir.File("current")).ok());
  EXPECT_FALSE(env()->FileExists(dir.File("new")));
  EXPECT_EQ(ReadWholeFile(dir.File("current")), "fresh");
  EXPECT_TRUE(env()->RenameFile(dir.File("gone"), dir.File("x")).IsNotFound());
}

TEST(PosixEnv, RemovesFiles) {
  const TestDir dir("env-remove");
  WriteFile(dir.File("f"), "x");
  ASSERT_TRUE(env()->RemoveFile(dir.File("f")).ok());
  EXPECT_FALSE(env()->FileExists(dir.File("f")));
}

TEST(PosixEnv, CreateDirIsIdempotentAndRemoveDirWorks) {
  const TestDir dir("env-dirs");
  const std::string sub = dir.File("sub");
  ASSERT_TRUE(env()->CreateDir(sub).ok());
  EXPECT_TRUE(env()->CreateDir(sub).ok());
  EXPECT_TRUE(env()->FileExists(sub));
  ASSERT_TRUE(env()->RemoveDir(sub).ok());
  EXPECT_FALSE(env()->FileExists(sub));
  EXPECT_TRUE(env()->RemoveDir(sub).IsNotFound());
}

TEST(PosixEnv, CreateDirFailsWhenAFileHasTheName) {
  const TestDir dir("env-dir-file");
  WriteFile(dir.File("f"), "x");
  EXPECT_FALSE(env()->CreateDir(dir.File("f")).ok());
}

TEST(PosixEnv, SyncDirSucceedsOnDirectoriesOnly) {
  const TestDir dir("env-syncdir");
  EXPECT_TRUE(env()->SyncDir(dir.path()).ok());
  EXPECT_TRUE(env()->SyncDir(dir.File("missing")).IsNotFound());
}

TEST(PosixEnv, LockIsExclusiveWithinOneProcess) {
  const TestDir dir("env-lock");
  auto first = env()->LockFile(dir.File("LOCK"));
  ASSERT_TRUE(first.ok()) << first.status().ToString();
  EXPECT_TRUE(env()->LockFile(dir.File("LOCK")).status().IsBusy());
  first->reset();
  auto again = env()->LockFile(dir.File("LOCK"));
  EXPECT_TRUE(again.ok()) << again.status().ToString();
}

// fcntl locks do not conflict inside one process, so the cross-process
// guarantee can only be checked from a second process.
int ChildTryLock(const std::string& path) {
  const pid_t child = ::fork();
  if (child == 0) {
    const int fd = ::open(path.c_str(), O_RDWR);
    struct flock request {};
    request.l_type = F_WRLCK;
    request.l_whence = SEEK_SET;
    const bool acquired = fd >= 0 && ::fcntl(fd, F_SETLK, &request) == 0;
    ::_exit(acquired ? 0 : 1);
  }
  int wait_status = 0;
  ::waitpid(child, &wait_status, 0);
  return WEXITSTATUS(wait_status);
}

TEST(PosixEnv, LockExcludesOtherProcessesUntilReleased) {
  const TestDir dir("env-lock-fork");
  const std::string path = dir.File("LOCK");
  auto lock = env()->LockFile(path);
  ASSERT_TRUE(lock.ok());
  EXPECT_EQ(ChildTryLock(path), 1) << "another process acquired a held lock";
  lock->reset();
  EXPECT_EQ(ChildTryLock(path), 0) << "lock was not released";
}

// The same file reached through a different spelling of its path, or through
// a symlink, is still the same lock. The failed attempt must also leave the
// original lock held: closing any descriptor for a file drops every fcntl lock
// this process holds on it, so a careless "already locked" path unlocks.
TEST(PosixEnv, LockIsKeyedOnTheFileNotThePathSpelling) {
  const TestDir dir("env-lock-alias");
  const std::string path = dir.File("LOCK");
  auto lock = env()->LockFile(path);
  ASSERT_TRUE(lock.ok());

  EXPECT_TRUE(env()->LockFile(dir.path() + "/./LOCK").status().IsBusy());
  ASSERT_EQ(::symlink(path.c_str(), dir.File("alias").c_str()), 0);
  EXPECT_TRUE(env()->LockFile(dir.File("alias")).status().IsBusy());

  EXPECT_EQ(ChildTryLock(path), 1) << "a rejected LockFile released the real lock";
}

// Once a write fails the file is poisoned: /dev/full fails every write with
// ENOSPC, and later calls must keep failing rather than report success.
TEST(PosixEnv, WriteErrorsAreSticky) {
  auto file = env()->NewAppendableFile("/dev/full");
  ASSERT_TRUE(file.ok()) << file.status().ToString();
  ASSERT_TRUE((*file)->Append("buffered").ok());  // still only in user space
  const strata::Status failed = (*file)->Flush();
  ASSERT_TRUE(failed.IsIoError()) << failed.ToString();
  EXPECT_TRUE((*file)->Append("more").IsIoError());
  EXPECT_TRUE((*file)->Flush().IsIoError());
  EXPECT_TRUE((*file)->Sync().IsIoError());
  EXPECT_TRUE((*file)->Close().IsIoError());
}

TEST(PosixEnv, LargeAppendErrorsAreSticky) {
  auto file = env()->NewAppendableFile("/dev/full");
  ASSERT_TRUE(file.ok());
  const std::string large(128 * 1024, 'x');  // bypasses the buffer
  EXPECT_TRUE((*file)->Append(large).IsIoError());
  EXPECT_TRUE((*file)->Append("x").IsIoError());
}

TEST(PosixEnv, NowMicrosIsMonotonic) {
  const std::uint64_t first = env()->NowMicros();
  const std::uint64_t second = env()->NowMicros();
  EXPECT_LE(first, second);
}

// EnvWrapper must forward to its target; checked through the observable
// effects of a representative set of calls.
TEST(EnvWrapper, ForwardsToTarget) {
  const TestDir dir("env-wrapper");
  strata::EnvWrapper wrapper(env());
  EXPECT_EQ(wrapper.target(), env());
  auto file = wrapper.NewWritableFile(dir.File("f"));
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE((*file)->Append("via wrapper").ok());
  ASSERT_TRUE((*file)->Close().ok());
  EXPECT_TRUE(wrapper.FileExists(dir.File("f")));
  EXPECT_EQ(*wrapper.GetFileSize(dir.File("f")), 11U);
  EXPECT_TRUE(wrapper.SyncDir(dir.path()).ok());
  ASSERT_TRUE(wrapper.RenameFile(dir.File("f"), dir.File("g")).ok());
  EXPECT_EQ(wrapper.GetChildren(dir.path())->size(), 1U);
  EXPECT_TRUE(wrapper.NewSequentialFile(dir.File("g")).ok());
  EXPECT_TRUE(wrapper.NewRandomAccessFile(dir.File("g")).ok());
  EXPECT_TRUE(wrapper.NewAppendableFile(dir.File("g")).ok());
  ASSERT_TRUE(wrapper.RemoveFile(dir.File("g")).ok());
  ASSERT_TRUE(wrapper.CreateDir(dir.File("d")).ok());
  ASSERT_TRUE(wrapper.RemoveDir(dir.File("d")).ok());
  auto lock = wrapper.LockFile(dir.File("LOCK"));
  EXPECT_TRUE(lock.ok());
  EXPECT_GT(wrapper.NowMicros(), 0U);
}

}  // namespace

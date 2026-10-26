// A memtable rotation must not stall readers.
//
// Rotating the write-ahead log fsyncs the retired log, which can hold up to
// write_buffer_size unsynced bytes and take tens of milliseconds. Phase 11's
// open-loop runs found that sync was done under the DB mutex, so every Get --
// which takes that mutex briefly -- waited for it. These tests park the
// rotating writer inside that sync and check what other threads can do
// meanwhile.

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "db/db_impl.hpp"
#include "support/test_dir.hpp"

namespace strata {
namespace {

using test::TestDir;

constexpr std::size_t kWriteBuffer = 64 * 1024;
// Enough 100-byte writes to fill kWriteBuffer several times over.
constexpr int kWritesToRotate = 4000;
constexpr auto kParkTimeout = std::chrono::seconds(10);
// Far longer than an uncontended Get, far shorter than "blocked until released".
constexpr auto kReadDeadline = std::chrono::seconds(2);

// Hands out files whose Sync, once armed, parks on a latch if the file is a
// write-ahead log. Only a rotation syncs a log when every write is unsynced,
// so the first parked Sync is the retired log's.
class ParkingSyncEnv final : public EnvWrapper {
 public:
  explicit ParkingSyncEnv(Env* target) : EnvWrapper(target) {}

  Result<std::unique_ptr<WritableFile>> NewWritableFile(const std::string& path) override {
    STRATA_ASSIGN_OR_RETURN(auto file, target()->NewWritableFile(path));
    const bool is_log = path.ends_with(".log");
    return std::unique_ptr<WritableFile>(std::make_unique<File>(this, std::move(file), is_log));
  }

  void Arm() {
    const std::lock_guard<std::mutex> guard(mutex_);
    armed_ = true;
  }

  // True once a log Sync is parked; false if none arrived in time.
  bool WaitUntilParked() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kParkTimeout, [this] { return parked_; });
  }

  void Release() {
    const std::lock_guard<std::mutex> guard(mutex_);
    armed_ = false;
    released_ = true;
    changed_.notify_all();
  }

 private:
  class File final : public WritableFile {
   public:
    File(ParkingSyncEnv* env, std::unique_ptr<WritableFile> file, bool is_log)
        : env_(env), file_(std::move(file)), is_log_(is_log) {}
    Status Append(Slice data) override { return file_->Append(data); }
    Status Flush() override { return file_->Flush(); }
    Status Sync() override {
      if (is_log_) {
        env_->MaybePark();
      }
      return file_->Sync();
    }
    Status Close() override { return file_->Close(); }

   private:
    ParkingSyncEnv* const env_;
    const std::unique_ptr<WritableFile> file_;
    const bool is_log_;
  };

  void MaybePark() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!armed_) {
      return;
    }
    parked_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return released_; });
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  bool armed_ = false;
  bool parked_ = false;
  bool released_ = false;
};

std::string Key(int index) {
  return "key" + std::to_string(100000 + index);
}

class RotationStallTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto opened =
        DBImpl::Open({.path = dir_.File("db"), .write_buffer_size = kWriteBuffer, .env = &env_});
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    db_ = std::move(*opened);
    WriteBatch batch;
    batch.Put("before", "rotation");
    ASSERT_TRUE(db_->Write(WriteOptions{}, batch).ok());
  }

  // Starts a writer that keeps writing until it has rotated the memtable and
  // is parked in the retired log's Sync.
  void ParkWriterInRotation() {
    env_.Arm();
    writer_ = std::thread([this] {
      for (int index = 0; index < kWritesToRotate; ++index) {
        WriteBatch batch;
        batch.Put(Key(index), std::string(100, 'v'));
        if (!db_->Write(WriteOptions{}, batch).ok()) {
          return;
        }
      }
    });
    ASSERT_TRUE(env_.WaitUntilParked()) << "the writer never rotated the log";
  }

  void TearDown() override {
    env_.Release();
    if (writer_.joinable()) {
      writer_.join();
    }
  }

  TestDir dir_{"rotation_stall"};
  ParkingSyncEnv env_{Env::Default()};
  std::unique_ptr<DBImpl> db_;
  std::thread writer_;
};

TEST_F(RotationStallTest, GetCompletesWhileRetiredLogSyncs) {
  ParkWriterInRotation();
  auto read = std::async(std::launch::async, [this] { return db_->Get(ReadOptions{}, "before"); });
  // Released before asserting: a blocked Get would otherwise keep the
  // future's destructor waiting forever.
  const bool in_time = read.wait_for(kReadDeadline) == std::future_status::ready;
  env_.Release();
  ASSERT_TRUE(in_time) << "Get waited for the rotating writer's fsync";
  const auto value = read.get();
  ASSERT_TRUE(value.ok()) << value.status().ToString();
  EXPECT_EQ(*value, "rotation");
}

TEST_F(RotationStallTest, SnapshotAndIteratorCompleteWhileRetiredLogSyncs) {
  ParkWriterInRotation();
  auto scan = std::async(std::launch::async, [this] {
    const Snapshot* snapshot = db_->GetSnapshot();
    auto it = db_->NewIterator(ReadOptions{.snapshot = snapshot});
    it->Seek("before");
    const bool found = it->Valid() && it->key() == Slice("before");
    it.reset();
    static_cast<void>(db_->ReleaseSnapshot(snapshot));
    return found;
  });
  const bool in_time = scan.wait_for(kReadDeadline) == std::future_status::ready;
  env_.Release();
  ASSERT_TRUE(in_time) << "the iterator waited for the rotating writer's fsync";
  EXPECT_TRUE(scan.get());
}

TEST_F(RotationStallTest, WritesAfterTheRotationLandInTheNewMemTable) {
  ParkWriterInRotation();
  env_.Release();
  writer_.join();
  for (int index = 0; index < kWritesToRotate; ++index) {
    const auto value = db_->Get(ReadOptions{}, Key(index));
    ASSERT_TRUE(value.ok()) << "write " << index << ": " << value.status().ToString();
  }
}

}  // namespace
}  // namespace strata

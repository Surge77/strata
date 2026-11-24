// Damage inside the newest WAL that is followed by intact records is not a
// torn write, even when everything sits in one partial block (the usual size
// of the newest log after a restart). A torn write leaves nothing valid after
// it; this does, and those records were acknowledged, so a strict Open must
// fail rather than drop them (ADR-0005).

#include "strata/db.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "support/test_dir.hpp"

namespace {

using strata::DB;
using strata::Options;
using strata::Status;
using strata::WriteOptions;
using strata::test::TestDir;

constexpr int kRecords = 100;
constexpr std::streamoff kFirstPayloadByte = 12;  // inside record 0's batch header
constexpr std::streamoff kFirstLengthField = 4;   // record 0's two length bytes
constexpr std::streamoff kHeaderSize = 7;

std::string Key(int index) {
  return "key" + std::to_string(1000 + index);
}

class WalDamageTest : public ::testing::Test {
 protected:
  Options StrictOptions() const { return Options{.path = dir_.File("db")}; }

  // Writes kRecords synced (acknowledged-durable) records into a log of one
  // partial block, closes, and returns that log.
  std::filesystem::path WriteSmallLog() {
    EXPECT_TRUE(DB::Open(StrictOptions(), &db_).ok());
    for (int index = 0; index < kRecords; ++index) {
      EXPECT_TRUE(db_.Put(Key(index), "v", WriteOptions{.sync = true}).ok());
    }
    EXPECT_TRUE(db_.Close().ok());
    std::vector<std::filesystem::path> logs;
    for (const auto& entry : std::filesystem::directory_iterator(dir_.File("db"))) {
      if (entry.path().extension() == ".log") {
        logs.push_back(entry.path());
      }
    }
    EXPECT_EQ(logs.size(), 1U);
    EXPECT_LT(std::filesystem::file_size(logs.front()), 32768U) << "must be one partial block";
    return logs.front();
  }

  static void Overwrite(const std::filesystem::path& file, std::streamoff offset,
                        const std::string& bytes) {
    std::fstream stream(file, std::ios::in | std::ios::out | std::ios::binary);
    stream.seekp(offset);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  void ExpectStrictOpenFails() {
    const Status status = DB::Open(StrictOptions(), &db_);
    EXPECT_TRUE(status.IsCorruption()) << status.ToString();
    if (db_.is_open()) {
      ADD_FAILURE() << "strict Open succeeded";
      static_cast<void>(db_.Close());
    }
  }

  // Salvage mode is the documented way past it (it drops the rest of the
  // damaged block), and what it recovers reopens strictly.
  void ExpectSalvageOpens() {
    Options salvage = StrictOptions();
    salvage.ignore_wal_corruption = true;
    const Status salvaged = DB::Open(salvage, &db_);
    ASSERT_TRUE(salvaged.ok()) << salvaged.ToString();
    ASSERT_TRUE(db_.Close().ok());
    const Status strict = DB::Open(StrictOptions(), &db_);
    EXPECT_TRUE(strict.ok()) << strict.ToString();
  }

  TestDir dir_{"wal_damage"};
  DB db_;
};

TEST_F(WalDamageTest, ChecksumDamageFollowedByRecordsInTheSameBlockFailsOpen) {
  const auto log = WriteSmallLog();
  Overwrite(log, kFirstPayloadByte, "\x7f");
  ExpectStrictOpenFails();
  ExpectSalvageOpens();
}

TEST_F(WalDamageTest, CorruptLengthFollowedByRecordsFailsOpen) {
  const auto log = WriteSmallLog();
  Overwrite(log, kFirstLengthField, "\xff\xff");  // claims to run past the end of the file
  ExpectStrictOpenFails();
  ExpectSalvageOpens();
}

TEST_F(WalDamageTest, ZeroedHeaderFollowedByRecordsFailsOpen) {
  const auto log = WriteSmallLog();
  Overwrite(log, 0, std::string(kHeaderSize, '\0'));  // reads as zero-filled space
  ExpectStrictOpenFails();
  ExpectSalvageOpens();
}

}  // namespace

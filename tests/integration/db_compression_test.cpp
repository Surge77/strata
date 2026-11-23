// Compression through the whole engine: flushes and compactions write
// compressed tables, and a database keeps reading every table whatever
// compression it is opened with now.

#include "strata/db.hpp"
#include "strata/iterator.hpp"
#include "strata/options.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "support/test_dir.hpp"

namespace strata {
namespace {

using test::TestDir;

constexpr int kKeys = 3000;

std::string Key(int i) {
  return "key" + std::to_string(100000 + i);
}

// Compressible, like most real values: a record with repeated field names.
std::string Value(int i, int round) {
  std::string value = "{\"round\":" + std::to_string(round) + ",\"id\":" + std::to_string(i);
  for (int field = 0; field < 8; ++field) {
    value += ",\"field" + std::to_string(field) + "\":\"some ordinary text value\"";
  }
  return value + "}";
}

std::uint64_t TableBytes(const std::string& path) {
  std::uint64_t total = 0;
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    if (entry.path().extension() == ".sst") {
      total += entry.file_size();
    }
  }
  return total;
}

// Writes kKeys values for `round` (keys `first` onward) and closes, so they
// reach tables.
void WriteRound(const std::string& path, Compression compression, int round, int first = 0) {
  DB db;
  ASSERT_TRUE(
      DB::Open({.path = path, .write_buffer_size = 64 * 1024, .compression = compression}, &db)
          .ok());
  for (int i = first; i < first + kKeys; ++i) {
    ASSERT_TRUE(db.Put(Key(i), Value(i, round)).ok());
  }
  ASSERT_TRUE(db.Close().ok());
}

void ExpectRound(const std::string& path, Compression compression, int round) {
  DB db;
  ASSERT_TRUE(DB::Open({.path = path, .compression = compression}, &db).ok());
  for (int i = 0; i < kKeys; i += 7) {
    const Result<std::string> value = db.Get(Key(i));
    ASSERT_TRUE(value.ok()) << Key(i) << ": " << value.status().ToString();
    EXPECT_EQ(*value, Value(i, round));
  }
  auto it = db.NewIterator();
  int count = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ++count;
  }
  EXPECT_TRUE(it->status().ok());
  EXPECT_EQ(count, kKeys);
  it.reset();
  ASSERT_TRUE(db.Close().ok());
}

TEST(DBCompressionTest, CompressedTablesAreSmallerAndReadBack) {
  TestDir plain_dir("compression-none");
  TestDir snappy_dir("compression-snappy");
  TestDir zstd_dir("compression-zstd");
  WriteRound(plain_dir.File("db"), Compression::kNone, 1);
  WriteRound(snappy_dir.File("db"), Compression::kSnappy, 1);
  WriteRound(zstd_dir.File("db"), Compression::kZstd, 1);
  ExpectRound(plain_dir.File("db"), Compression::kNone, 1);
  ExpectRound(snappy_dir.File("db"), Compression::kSnappy, 1);
  ExpectRound(zstd_dir.File("db"), Compression::kZstd, 1);
  const std::uint64_t plain = TableBytes(plain_dir.File("db"));
  EXPECT_LT(TableBytes(snappy_dir.File("db")), plain / 2);
  EXPECT_LT(TableBytes(zstd_dir.File("db")), plain / 2);
}

// Each block names its own encoding, so changing the setting never strands
// tables written under the old one. Each round writes its own keys, so every
// round's data must really be read back (nothing is shadowed by a later
// round), whatever setting the database is reopened with. Compactions during
// later rounds re-encode some earlier tables under the newer setting, so the
// database ends up mixed. That each setting produces compressed tables at
// all is CompressedTablesAreSmallerAndReadBack's job.
TEST(DBCompressionTest, ADatabaseReadsTablesWrittenUnderEverySetting) {
  TestDir dir("compression-mixed");
  const std::string path = dir.File("db");
  WriteRound(path, Compression::kZstd, 1, 0);
  WriteRound(path, Compression::kSnappy, 2, kKeys);
  WriteRound(path, Compression::kNone, 3, 2 * kKeys);
  for (const Compression reopened : {Compression::kZstd, Compression::kNone}) {
    DB db;
    ASSERT_TRUE(DB::Open({.path = path, .compression = reopened}, &db).ok());
    for (int round = 1; round <= 3; ++round) {
      for (int i = (round - 1) * kKeys; i < round * kKeys; i += 7) {
        const Result<std::string> value = db.Get(Key(i));
        ASSERT_TRUE(value.ok()) << "round " << round << " " << Key(i);
        EXPECT_EQ(*value, Value(i, round));
      }
    }
    ASSERT_TRUE(db.Close().ok());
  }
}

TEST(DBCompressionTest, ZstdLevelsTradeSpaceForTime) {
  TestDir fast_dir("compression-zstd-1");
  TestDir small_dir("compression-zstd-19");
  for (const auto& [dir, level] : {std::pair{&fast_dir, 1}, std::pair{&small_dir, 19}}) {
    DB db;
    // A small write buffer, so the records reach tables rather than staying
    // in the log (where compression never applies).
    ASSERT_TRUE(DB::Open({.path = dir->File("db"),
                          .write_buffer_size = 64 * 1024,
                          .compression = Compression::kZstd,
                          .compression_level = level},
                         &db)
                    .ok());
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db.Put(Key(i), Value(i, 1)).ok());
    }
    ASSERT_TRUE(db.Close().ok());
  }
  EXPECT_LT(TableBytes(small_dir.File("db")), TableBytes(fast_dir.File("db")))
      << "level 19 must be smaller than level 1 on repetitive records";
  ExpectRound(small_dir.File("db"), Compression::kNone, 1);
}

}  // namespace
}  // namespace strata

#include "table/table.hpp"

#include <map>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "support/file_util.hpp"
#include "support/test_dir.hpp"
#include "table/table_builder.hpp"
#include "util/comparator.hpp"

namespace {

using strata::BytewiseComparator;
using strata::Env;
using strata::Slice;
using strata::Status;
using strata::table::Table;
using strata::table::TableBuilder;
using strata::table::TableOptions;
using strata::test::TestDir;

using Entries = std::map<std::string, std::string>;

TableOptions Options(std::size_t block_size = 256) {
  return TableOptions{.comparator = BytewiseComparator(), .block_size = block_size};
}

Entries RandomEntries(std::size_t count, std::uint32_t seed) {
  std::mt19937 random(seed);
  Entries entries;
  while (entries.size() < count) {
    entries["key" + std::to_string(random() % 1000000)] =
        std::string(random() % 100, static_cast<char>('a' + random() % 26));
  }
  return entries;
}

std::uint64_t Build(const std::string& path, const Entries& entries, TableOptions options) {
  auto file = Env::Default()->NewWritableFile(path);
  EXPECT_TRUE(file.ok());
  TableBuilder builder(options, file->get());
  for (const auto& [key, value] : entries) {
    EXPECT_TRUE(builder.Add(key, value).ok());
  }
  EXPECT_TRUE(builder.Finish().ok());
  EXPECT_EQ(builder.NumEntries(), entries.size());
  EXPECT_TRUE((*file)->Close().ok());
  return builder.FileSize();
}

strata::Result<std::unique_ptr<Table>> OpenTable(const std::string& path,
                                                 TableOptions options = Options()) {
  auto file = Env::Default()->NewRandomAccessFile(path);
  if (!file.ok()) {
    return file.status();
  }
  auto size = Env::Default()->GetFileSize(path);
  return Table::Open(options, std::move(file).value(), *size);
}

class TableTest : public ::testing::Test {
 protected:
  TestDir dir_{"table"};
  std::string path_ = dir_.File("000001.sst");
};

TEST_F(TableTest, FileSizeMatchesTheFile) {
  const std::uint64_t size = Build(path_, RandomEntries(100, 1), Options());
  EXPECT_EQ(size, *Env::Default()->GetFileSize(path_));
}

TEST_F(TableTest, EmptyTableOpensAndIteratesNothing) {
  Build(path_, {}, Options());
  auto table = OpenTable(path_);
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  auto it = (*table)->NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

class TableRoundTrip : public TableTest, public ::testing::WithParamInterface<std::size_t> {};

// Block sizes from "one entry per block" to "everything in one block".
TEST_P(TableRoundTrip, IteratesSeeksAndReversesExactly) {
  const Entries entries = RandomEntries(2000, 7);
  Build(path_, entries, Options(GetParam()));
  auto table = OpenTable(path_, Options(GetParam()));
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  auto it = (*table)->NewIterator();

  auto expected = entries.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++expected) {
    ASSERT_NE(expected, entries.end());
    ASSERT_EQ(it->key(), expected->first);
    ASSERT_EQ(it->value(), expected->second);
  }
  EXPECT_EQ(expected, entries.end());
  EXPECT_TRUE(it->status().ok());

  auto reverse = entries.rbegin();
  for (it->SeekToLast(); it->Valid(); it->Prev(), ++reverse) {
    ASSERT_NE(reverse, entries.rend());
    ASSERT_EQ(it->key(), reverse->first);
  }
  EXPECT_EQ(reverse, entries.rend());

  std::mt19937 random(3);
  for (int probe = 0; probe < 2000; ++probe) {
    const std::string target = "key" + std::to_string(random() % 1000000);
    const auto lower = entries.lower_bound(target);
    std::string found_key;
    ASSERT_TRUE((*table)->Seek(target, [&](Slice key, Slice) { found_key.assign(key); }).ok());
    if (lower == entries.end()) {
      ASSERT_TRUE(found_key.empty()) << target;
    } else {
      ASSERT_EQ(found_key, lower->first) << target;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(BlockSizes, TableRoundTrip,
                         ::testing::Values(std::size_t{1}, std::size_t{256}, std::size_t{4096},
                                           std::size_t{1} << 30));

TEST_F(TableTest, RejectsKeysOutOfOrder) {
  auto file = Env::Default()->NewWritableFile(path_);
  TableBuilder builder(Options(), file->get());
  ASSERT_TRUE(builder.Add("b", "1").ok());
  EXPECT_TRUE(builder.Add("a", "2").IsInvalidArgument());
  EXPECT_TRUE(builder.Add("c", "3").IsInvalidArgument()) << "the error sticks";
  EXPECT_TRUE(builder.Finish().IsInvalidArgument());
}

TEST_F(TableTest, RejectsDuplicateKeys) {
  auto file = Env::Default()->NewWritableFile(path_);
  TableBuilder builder(Options(), file->get());
  ASSERT_TRUE(builder.Add("a", "1").ok());
  EXPECT_TRUE(builder.Add("a", "2").IsInvalidArgument());
  builder.Abandon();
}

TEST_F(TableTest, ApproximateOffsetsGrowWithKeys) {
  Entries entries;
  for (int index = 0; index < 1000; ++index) {
    entries["k" + std::to_string(10000 + index)] = std::string(1000, 'v');
  }
  Build(path_, entries, Options(4096));
  auto table = OpenTable(path_, Options(4096));
  ASSERT_TRUE(table.ok());
  const std::uint64_t early = (*table)->ApproximateOffsetOf("k10100");
  const std::uint64_t late = (*table)->ApproximateOffsetOf("k10900");
  const std::uint64_t past_end = (*table)->ApproximateOffsetOf("z");
  EXPECT_LT(early, late);
  EXPECT_LE(late, past_end);
  EXPECT_NEAR(static_cast<double>(early), 100.0 * 1000, 20000.0);
  EXPECT_NEAR(static_cast<double>(past_end), 1000.0 * 1000, 50000.0);
}

TEST_F(TableTest, RejectsFilesThatAreNotTables) {
  {
    auto file = Env::Default()->NewWritableFile(path_);
    ASSERT_TRUE((*file)->Append("too short").ok());
    ASSERT_TRUE((*file)->Close().ok());
  }
  EXPECT_TRUE(OpenTable(path_).status().IsCorruption());
  {
    auto file = Env::Default()->NewWritableFile(path_);
    ASSERT_TRUE((*file)->Append(std::string(100, 'x')).ok());
    ASSERT_TRUE((*file)->Close().ok());
  }
  EXPECT_TRUE(OpenTable(path_).status().IsCorruption());
}

// The phase-4 merge gate: corrupt every byte of a real table, one at a time.
// For every damaged copy, the reader must never return data that is not in
// the original table, and if it returns less than the whole table it must say
// so through status() -- or refuse to open at all. Under ASan this is also
// the memory-safety proof for the whole read path.
TEST_F(TableTest, EveryCorruptedByteIsDetectedOrHarmless) {
  const Entries entries = RandomEntries(40, 11);
  Build(path_, entries, Options(128));
  const std::string pristine = strata::test::ReadFileToString(path_);
  const std::string damaged_path = dir_.File("000002.sst");

  std::size_t rejected_at_open = 0;
  std::size_t reported_while_reading = 0;
  for (std::size_t offset = 0; offset < pristine.size(); ++offset) {
    std::string damaged = pristine;
    damaged[offset] = static_cast<char>(static_cast<unsigned char>(damaged[offset]) ^ 0x5AU);
    {
      auto file = Env::Default()->NewWritableFile(damaged_path);
      ASSERT_TRUE((*file)->Append(damaged).ok());
      ASSERT_TRUE((*file)->Close().ok());
    }
    auto table = OpenTable(damaged_path, Options(128));
    if (!table.ok()) {
      ASSERT_TRUE(table.status().IsCorruption()) << offset << ": " << table.status().ToString();
      ++rejected_at_open;
      continue;
    }
    auto it = (*table)->NewIterator();
    std::size_t returned = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const auto match = entries.find(std::string(it->key()));
      ASSERT_NE(match, entries.end()) << "invented key at offset " << offset;
      ASSERT_EQ(it->value(), match->second) << "wrong value at offset " << offset;
      ++returned;
    }
    if (returned < entries.size()) {
      ASSERT_FALSE(it->status().ok()) << "silently lost entries at offset " << offset;
      ++reported_while_reading;
    }
  }
  // Every byte of the file is load-bearing except the footer's padding, so
  // nearly all damage must be caught one way or the other.
  EXPECT_GT(rejected_at_open + reported_while_reading, pristine.size() * 9 / 10);
}

}  // namespace

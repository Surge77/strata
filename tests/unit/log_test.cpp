#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "log/log_format.hpp"
#include "log/log_reader.hpp"
#include "log/log_writer.hpp"
#include "support/string_file.hpp"
#include "util/coding.hpp"
#include "util/crc32c.hpp"

namespace {

using strata::Slice;
using strata::Status;
using strata::log::kBlockSize;
using strata::log::kHeaderSize;
using strata::log::Reader;
using strata::log::RecordType;
using strata::log::Writer;
using strata::test::StringSink;
using strata::test::StringSource;

// Records every drop the reader reports.
class CountingReporter final : public Reader::Reporter {
 public:
  void Corruption(std::size_t bytes_dropped, const Status& reason) override {
    dropped_bytes += bytes_dropped;
    reasons.emplace_back(reason.ToString());
  }
  std::size_t dropped_bytes = 0;
  std::vector<std::string> reasons;
};

// A record of `length` bytes whose content encodes its own index, so a reader
// returning records in the wrong order or mixing fragments is caught.
std::string MakeRecord(std::size_t index, std::size_t length) {
  std::string record(length, '\0');
  for (std::size_t offset = 0; offset < length; ++offset) {
    record[offset] = static_cast<char>((index * 31U + offset) % 251U);
  }
  return record;
}

std::string WriteLog(const std::vector<std::string>& records) {
  std::string contents;
  StringSink sink(&contents);
  Writer writer(&sink);
  for (const auto& record : records) {
    EXPECT_TRUE(writer.AddRecord(record).ok());
  }
  return contents;
}

std::vector<std::string> ReadLog(const std::string& contents, CountingReporter* reporter) {
  StringSource source(contents);
  Reader reader(&source, reporter);
  std::vector<std::string> records;
  Slice record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
    records.emplace_back(record);
  }
  return records;
}

// Writes a fragment exactly as the spec describes, for tests that need to hand
// the reader bytes the writer would never produce.
std::string RawFragment(RecordType type, Slice payload) {
  std::string fragment(kHeaderSize, '\0');
  const char type_byte = static_cast<char>(type);
  std::uint32_t crc = strata::crc32c::Value(Slice(&type_byte, 1));
  crc = strata::crc32c::Extend(crc, payload);
  strata::EncodeFixed32(fragment.data(), strata::crc32c::Mask(crc));
  fragment[4] = static_cast<char>(payload.size() & 0xFFU);
  fragment[5] = static_cast<char>(payload.size() >> 8U);
  fragment[6] = type_byte;
  fragment.append(payload);
  return fragment;
}

TEST(Log, EmptyLogHasNoRecords) {
  CountingReporter reporter;
  EXPECT_TRUE(ReadLog("", &reporter).empty());
  EXPECT_TRUE(reporter.reasons.empty());
}

TEST(Log, RoundTripsSmallAndEmptyRecords) {
  const std::vector<std::string> records = {"foo", "", "bar", std::string(1000, 'x')};
  CountingReporter reporter;
  EXPECT_EQ(ReadLog(WriteLog(records), &reporter), records);
  EXPECT_TRUE(reporter.reasons.empty());
}

TEST(Log, EmptyRecordIsASingleHeader) {
  EXPECT_EQ(WriteLog({""}).size(), kHeaderSize);
}

// Sizes chosen around every boundary the writer has to handle: exactly filling
// a block, one byte either side, leaving 0..7 bytes of trailer, and records
// spanning several blocks.
TEST(Log, RoundTripsRecordsAroundBlockBoundaries) {
  std::vector<std::string> records;
  std::size_t index = 0;
  for (const std::size_t length :
       {kBlockSize - kHeaderSize, kBlockSize - kHeaderSize - 1, kBlockSize - kHeaderSize + 1,
        kBlockSize - 2 * kHeaderSize, kBlockSize - 2 * kHeaderSize + 1, std::size_t{1},
        3 * kBlockSize, 3 * kBlockSize + 7, std::size_t{0}, kBlockSize}) {
    records.push_back(MakeRecord(index++, length));
  }
  CountingReporter reporter;
  EXPECT_EQ(ReadLog(WriteLog(records), &reporter), records);
  EXPECT_TRUE(reporter.reasons.empty());
}

// Leaves exactly 6 bytes in the first block: too few for a header, so the
// writer must pad with zeros and the next record starts in block 1.
TEST(Log, PadsATrailerTooSmallForAHeader) {
  const std::string first = MakeRecord(0, kBlockSize - 2 * kHeaderSize + 1);
  const std::string contents = WriteLog({first, "next"});
  ASSERT_EQ(contents.size(), kBlockSize + kHeaderSize + 4);
  for (std::size_t offset = kBlockSize - 6; offset < kBlockSize; ++offset) {
    EXPECT_EQ(contents[offset], '\0') << "trailer byte " << offset;
  }
  CountingReporter reporter;
  EXPECT_EQ(ReadLog(contents, &reporter), (std::vector<std::string>{first, "next"}));
  EXPECT_TRUE(reporter.reasons.empty());
}

TEST(Log, RoundTripsManyRandomRecords) {
  std::mt19937 random(301);
  std::vector<std::string> records;
  for (std::size_t index = 0; index < 2000; ++index) {
    // Mostly small, occasionally multi-block: the skew real WAL traffic has.
    const std::size_t length = random() % 10 == 0 ? random() % (3 * kBlockSize) : random() % 200;
    records.push_back(MakeRecord(index, length));
  }
  CountingReporter reporter;
  EXPECT_EQ(ReadLog(WriteLog(records), &reporter), records);
  EXPECT_TRUE(reporter.reasons.empty());
}

// The worked example in docs/format/log.md, byte for byte.
TEST(Log, MatchesTheSpecWorkedExample) {
  const std::string contents = WriteLog({MakeRecord(0, 100), MakeRecord(1, 40000)});
  ASSERT_EQ(contents.size(), 32775U + 7346U);
  EXPECT_EQ(static_cast<std::uint8_t>(contents[6]), 1U);          // kFull
  EXPECT_EQ(static_cast<std::uint8_t>(contents[107 + 6]), 2U);    // kFirst
  EXPECT_EQ(static_cast<std::uint8_t>(contents[32768 + 6]), 4U);  // kLast
  const auto first_length = static_cast<std::uint8_t>(contents[107 + 4]) |
                            (static_cast<std::uint8_t>(contents[107 + 5]) << 8);
  EXPECT_EQ(first_length, 32654);
}

TEST(Log, ReportsTheOffsetOfEachRecord) {
  const std::string contents = WriteLog({MakeRecord(0, 100), MakeRecord(1, 40000), "tail"});
  StringSource source(contents);
  Reader reader(&source, nullptr);
  Slice record;
  std::string scratch;
  std::vector<std::uint64_t> offsets;
  while (reader.ReadRecord(&record, &scratch)) {
    offsets.push_back(reader.LastRecordOffset());
  }
  EXPECT_EQ(offsets, (std::vector<std::uint64_t>{0, 107, 32768 + 7 + 7346}));
}

// The merge gate for this phase. A crash can cut the log at any byte; for
// every possible cut the reader must return exactly the records that were
// complete before it, in order, and report nothing -- a torn tail is not
// corruption.
TEST(Log, EveryTruncationOffsetYieldsACleanPrefix) {
  std::vector<std::string> records;
  std::vector<std::size_t> ends;  // file offset at which record i is complete
  std::string contents;
  StringSink sink(&contents);
  Writer writer(&sink);
  std::mt19937 random(17);
  for (std::size_t index = 0; index < 60; ++index) {
    // Three multi-block records among small ones: enough to cut inside every
    // fragment type, while keeping this O(size^2) test to about a second.
    const std::size_t length = index % 20 == 0 ? kBlockSize + random() % 2000 : random() % 300;
    records.push_back(MakeRecord(index, length));
    ASSERT_TRUE(writer.AddRecord(records.back()).ok());
    ends.push_back(contents.size());
  }

  for (std::size_t cut = 0; cut <= contents.size(); ++cut) {
    CountingReporter reporter;
    const auto read = ReadLog(contents.substr(0, cut), &reporter);
    std::size_t complete = 0;
    while (complete < ends.size() && ends[complete] <= cut) {
      ++complete;
    }
    ASSERT_EQ(read.size(), complete) << "cut at " << cut;
    for (std::size_t index = 0; index < complete; ++index) {
      ASSERT_EQ(read[index], records[index]) << "cut at " << cut << " record " << index;
    }
    ASSERT_TRUE(reporter.reasons.empty()) << "cut at " << cut << ": " << reporter.reasons[0];
  }
}

TEST(Log, ChecksumMismatchSkipsTheRestOfTheBlockAndResumes) {
  // Two small records in block 0, then one record that starts in block 1.
  std::vector<std::string> records = {"first", "second", MakeRecord(2, kBlockSize)};
  std::string contents = WriteLog(records);
  contents[kHeaderSize + 1] ^= 0x01;  // corrupt the payload of "first"

  CountingReporter reporter;
  const auto read = ReadLog(contents, &reporter);
  // "first" fails its checksum; "second" shares its block and is dropped with
  // it; the big record's kFirst fragment is also in block 0, so its later
  // fragments arrive without a start and are dropped too.
  ASSERT_FALSE(reporter.reasons.empty());
  EXPECT_NE(reporter.reasons[0].find("checksum mismatch"), std::string::npos);
  EXPECT_TRUE(read.empty());
}

TEST(Log, CorruptionInOneBlockDoesNotCostLaterBlocks) {
  std::vector<std::string> records;
  for (std::size_t index = 0; index < 10; ++index) {
    records.push_back(MakeRecord(index, kBlockSize - kHeaderSize));  // one per block
  }
  std::string contents = WriteLog(records);
  contents[3 * kBlockSize + kHeaderSize + 10] ^= 0x40;  // damage record 3

  CountingReporter reporter;
  const auto read = ReadLog(contents, &reporter);
  std::vector<std::string> expected = records;
  expected.erase(expected.begin() + 3);
  EXPECT_EQ(read, expected);
  EXPECT_EQ(reporter.dropped_bytes, kBlockSize);
}

TEST(Log, CorruptTypeByteIsCaughtByTheChecksum) {
  std::string contents = WriteLog({"payload", "other"});
  contents[6] = static_cast<char>(RecordType::kLast);  // was kFull
  CountingReporter reporter;
  const auto read = ReadLog(contents, &reporter);
  EXPECT_TRUE(read.empty());
  ASSERT_EQ(reporter.reasons.size(), 1U);
  EXPECT_NE(reporter.reasons[0].find("checksum mismatch"), std::string::npos);
}

TEST(Log, BadLengthInsideTheFileIsReported) {
  std::vector<std::string> records = {"short", MakeRecord(1, kBlockSize), "after"};
  std::string contents = WriteLog(records);
  contents[4] = static_cast<char>(0xFF);  // length of "short" now runs off the block
  contents[5] = static_cast<char>(0x7F);
  CountingReporter reporter;
  const auto read = ReadLog(contents, &reporter);
  ASSERT_FALSE(reporter.reasons.empty());
  EXPECT_NE(reporter.reasons[0].find("bad record length"), std::string::npos);
  EXPECT_EQ(read.back(), "after");
}

TEST(Log, ZeroFilledSpaceIsSkippedSilently) {
  std::string contents(kBlockSize, '\0');  // a preallocated, never-written block
  contents += RawFragment(RecordType::kFull, "real");
  CountingReporter reporter;
  EXPECT_EQ(ReadLog(contents, &reporter), (std::vector<std::string>{"real"}));
  EXPECT_TRUE(reporter.reasons.empty());
}

TEST(Log, FragmentsWithoutAStartAreReported) {
  const std::string contents = RawFragment(RecordType::kMiddle, "mid") +
                               RawFragment(RecordType::kLast, "end") +
                               RawFragment(RecordType::kFull, "whole");
  CountingReporter reporter;
  EXPECT_EQ(ReadLog(contents, &reporter), (std::vector<std::string>{"whole"}));
  EXPECT_EQ(reporter.reasons.size(), 2U);
  EXPECT_EQ(reporter.dropped_bytes, 6U);
}

TEST(Log, ARecordStartedTwiceReportsTheAbandonedPart) {
  const std::string contents =
      RawFragment(RecordType::kFirst, "abandoned") + RawFragment(RecordType::kFirst, "real-") +
      RawFragment(RecordType::kLast, "record") + RawFragment(RecordType::kFirst, "again") +
      RawFragment(RecordType::kFull, "whole");
  CountingReporter reporter;
  EXPECT_EQ(ReadLog(contents, &reporter), (std::vector<std::string>{"real-record", "whole"}));
  EXPECT_EQ(reporter.dropped_bytes, 9U + 5U);
}

TEST(Log, UnknownTypesAreReported) {
  std::string contents = RawFragment(RecordType::kFull, "ok");
  // Type 9 with a valid checksum: only the type check can reject it.
  std::string bogus(kHeaderSize, '\0');
  const char type_byte = 9;
  strata::EncodeFixed32(bogus.data(),
                        strata::crc32c::Mask(strata::crc32c::Value(Slice(&type_byte, 1))));
  bogus[6] = type_byte;
  contents += bogus + RawFragment(RecordType::kFull, "after");
  CountingReporter reporter;
  EXPECT_EQ(ReadLog(contents, &reporter), (std::vector<std::string>{"ok", "after"}));
  ASSERT_EQ(reporter.reasons.size(), 1U);
  EXPECT_NE(reporter.reasons[0].find("unknown record type"), std::string::npos);
}

TEST(Log, AbandonedFragmentFollowedByCorruptionIsReported) {
  std::string contents = RawFragment(RecordType::kFirst, "start");
  std::string damaged = RawFragment(RecordType::kLast, "end");
  damaged[kHeaderSize] ^= 0x01;
  contents += damaged;
  CountingReporter reporter;
  EXPECT_TRUE(ReadLog(contents, &reporter).empty());
  EXPECT_EQ(reporter.reasons.size(), 2U);  // the checksum, then the abandoned start
}

TEST(Log, ReadErrorEndsTheLogAndIsReportedAsIoError) {
  StringSource source(WriteLog({"a"}));
  source.FailReads();
  CountingReporter reporter;
  Reader reader(&source, &reporter);
  Slice record;
  std::string scratch;
  EXPECT_FALSE(reader.ReadRecord(&record, &scratch));
  ASSERT_EQ(reporter.reasons.size(), 1U);
  EXPECT_NE(reporter.reasons[0].find("IoError"), std::string::npos);
}

TEST(Log, NullReporterIsAllowed) {
  std::string contents = WriteLog({"x", "y"});
  contents[kHeaderSize] ^= 0x01;
  StringSource source(contents);
  Reader reader(&source, nullptr);
  Slice record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
  }
  SUCCEED();
}

// Reopening an existing log for append must continue at the right position
// within the current block, or fragments would straddle block boundaries.
TEST(Log, ResumesAnExistingLogAtTheRightBlockOffset) {
  std::vector<std::string> records = {MakeRecord(0, kBlockSize - 100), MakeRecord(1, 500)};
  std::string contents = WriteLog(records);
  {
    StringSink sink(&contents);
    Writer writer(&sink, contents.size());
    records.push_back(MakeRecord(2, 2 * kBlockSize));
    ASSERT_TRUE(writer.AddRecord(records.back()).ok());
  }
  CountingReporter reporter;
  EXPECT_EQ(ReadLog(contents, &reporter), records);
  EXPECT_TRUE(reporter.reasons.empty());
}

// After a failed append the file may hold a partial fragment. The writer must
// refuse further records rather than write them misframed after it.
TEST(Log, WriterIsPoisonedAfterAFailure) {
  std::string contents;
  StringSink sink(&contents);
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord("fits").ok());
  sink.FailAfter(contents.size() + kHeaderSize + 2);
  EXPECT_TRUE(writer.AddRecord("this one fails partway").IsIoError());
  sink.FailAfter(static_cast<std::size_t>(-1));
  EXPECT_TRUE(writer.AddRecord("never written").IsIoError());
}

// A failed flush leaves the record's bytes in an unknown state: poisoned too.
TEST(Log, FlushFailurePoisonsTheWriter) {
  std::string contents;
  StringSink sink(&contents);
  Writer writer(&sink);
  sink.FailFlush();
  EXPECT_TRUE(writer.AddRecord("unflushed").IsIoError());
  EXPECT_TRUE(writer.AddRecord("never").IsIoError());
}

TEST(Log, PaddingFailurePoisonsTheWriter) {
  std::string contents;
  StringSink sink(&contents);
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord(MakeRecord(0, kBlockSize - 2 * kHeaderSize + 1)).ok());
  sink.FailAfter(contents.size());  // the 6-byte trailer cannot be written
  EXPECT_TRUE(writer.AddRecord("x").IsIoError());
}

}  // namespace

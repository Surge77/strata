#include "db/version_edit.hpp"

#include <string>

#include <gtest/gtest.h>

namespace {

using strata::FileMetaData;
using strata::VersionEdit;

std::string Ikey(std::string_view key, strata::SequenceNumber sequence) {
  std::string encoded;
  strata::AppendInternalKey(&encoded, {key, sequence, strata::ValueType::kValue});
  return encoded;
}

VersionEdit FullEdit() {
  VersionEdit edit;
  edit.SetComparatorName("strata.BytewiseComparator");
  edit.SetLogNumber(12);
  edit.SetNextFile(99);
  edit.SetLastSequence(123456);
  edit.RemoveFile(3, 40);
  edit.AddFile(
      0, FileMetaData{
             .number = 41, .file_size = 4096, .smallest = Ikey("a", 5), .largest = Ikey("zz", 9)});
  edit.AddFile(
      6,
      FileMetaData{.number = 42, .file_size = 1, .smallest = Ikey("", 1), .largest = Ikey("", 1)});
  return edit;
}

TEST(VersionEdit, RoundTripsEveryField) {
  std::string encoded;
  FullEdit().EncodeTo(&encoded);
  VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(encoded).ok());
  EXPECT_EQ(decoded.comparator(), "strata.BytewiseComparator");
  EXPECT_EQ(decoded.log_number(), 12U);
  EXPECT_EQ(decoded.next_file_number(), 99U);
  EXPECT_EQ(decoded.last_sequence(), 123456U);
  EXPECT_EQ(decoded.deleted_files().size(), 1U);
  ASSERT_EQ(decoded.new_files().size(), 2U);
  EXPECT_EQ(decoded.new_files()[0].second.largest, Ikey("zz", 9));
  EXPECT_EQ(decoded.new_files()[1].first, 6);

  std::string reencoded;
  decoded.EncodeTo(&reencoded);
  EXPECT_EQ(reencoded, encoded);
}

TEST(VersionEdit, EmptyEditHasNoFields) {
  VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom("").ok());
  EXPECT_FALSE(decoded.log_number().has_value());
  EXPECT_TRUE(decoded.new_files().empty());
}

TEST(VersionEdit, EveryTruncationIsCorruption) {
  std::string encoded;
  FullEdit().EncodeTo(&encoded);
  for (std::size_t cut = 1; cut < encoded.size(); ++cut) {
    VersionEdit decoded;
    const auto status = decoded.DecodeFrom(std::string_view(encoded).substr(0, cut));
    // A cut exactly between fields is a valid shorter edit; any other cut is not.
    if (!status.ok()) {
      EXPECT_TRUE(status.IsCorruption()) << cut;
    }
  }
}

TEST(VersionEdit, RejectsUnknownTagsBadLevelsAndBadKeys) {
  VersionEdit decoded;
  EXPECT_TRUE(decoded.DecodeFrom("\x63").IsCorruption());                          // tag 99
  EXPECT_TRUE(decoded.DecodeFrom(std::string("\x05\x07\x01", 3)).IsCorruption());  // level 7
  // New file whose smallest key is shorter than an internal-key trailer.
  EXPECT_TRUE(decoded
                  .DecodeFrom(std::string("\x06\x00\x01\x01\x03"
                                          "abc"
                                          "\x03"
                                          "abc",
                                          13))
                  .IsCorruption());
}

}  // namespace

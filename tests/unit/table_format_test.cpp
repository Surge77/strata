#include <string>

#include <gtest/gtest.h>

#include "support/test_dir.hpp"
#include "table/format.hpp"
#include "util/coding.hpp"
#include "util/crc32c.hpp"

namespace {

using strata::Env;
using strata::Slice;
using strata::table::BlockHandle;
using strata::table::Footer;
using strata::test::TestDir;

TEST(BlockHandle, RoundTripsAndRejectsTruncation) {
  const BlockHandle handle(123456789, 4096);
  std::string encoded;
  handle.EncodeTo(&encoded);
  EXPECT_LE(encoded.size(), BlockHandle::kMaxEncodedLength);

  Slice input(encoded);
  BlockHandle decoded;
  ASSERT_TRUE(decoded.DecodeFrom(&input).ok());
  EXPECT_EQ(decoded.offset(), 123456789U);
  EXPECT_EQ(decoded.size(), 4096U);
  EXPECT_TRUE(input.empty());

  for (std::size_t cut = 0; cut < encoded.size(); ++cut) {
    Slice truncated = Slice(encoded).substr(0, cut);
    EXPECT_TRUE(decoded.DecodeFrom(&truncated).IsCorruption()) << cut;
  }
}

TEST(Footer, RoundTripsAtItsFixedSize) {
  Footer footer;
  footer.metaindex_handle = BlockHandle(1000, 20);
  footer.index_handle = BlockHandle(1025, 300);
  std::string encoded;
  footer.EncodeTo(&encoded);
  ASSERT_EQ(encoded.size(), Footer::kEncodedLength);
  EXPECT_EQ(encoded.substr(40), std::string("TSATARTS"))
      << "the magic number is \"STRATAST\" stored little-endian";

  Footer decoded;
  ASSERT_TRUE(decoded.DecodeFrom(encoded).ok());
  EXPECT_EQ(decoded.metaindex_handle.offset(), 1000U);
  EXPECT_EQ(decoded.index_handle.size(), 300U);
}

TEST(Footer, RejectsWrongSizeAndWrongMagic) {
  Footer footer;
  std::string encoded;
  footer.EncodeTo(&encoded);
  Footer decoded;
  EXPECT_TRUE(decoded.DecodeFrom(Slice(encoded).substr(1)).IsCorruption());
  std::string bad_magic = encoded;
  bad_magic.back() ^= 0x01;
  EXPECT_TRUE(decoded.DecodeFrom(bad_magic).IsCorruption());
}

// Writes `contents` followed by a correct trailer, returns the file size.
std::uint64_t WriteBlockFile(const std::string& path, Slice contents, char type = 0) {
  std::string bytes(contents);
  bytes.push_back(type);
  const std::uint32_t crc = strata::crc32c::Value(bytes);
  strata::PutFixed32(&bytes, strata::crc32c::Mask(crc));
  auto file = Env::Default()->NewWritableFile(path);
  EXPECT_TRUE(file.ok());
  EXPECT_TRUE((*file)->Append(bytes).ok());
  EXPECT_TRUE((*file)->Close().ok());
  return bytes.size();
}

class ReadBlockTest : public ::testing::Test {
 protected:
  std::unique_ptr<strata::RandomAccessFile> OpenFile() {
    auto file = Env::Default()->NewRandomAccessFile(path_);
    EXPECT_TRUE(file.ok());
    return std::move(file).value();
  }
  TestDir dir_{"read-block"};
  std::string path_ = dir_.File("block");
};

TEST_F(ReadBlockTest, ReturnsVerifiedContents) {
  const std::uint64_t size = WriteBlockFile(path_, "block contents");
  const auto file = OpenFile();
  auto contents = strata::table::ReadBlock(*file, size, BlockHandle(0, 14));
  ASSERT_TRUE(contents.ok()) << contents.status().ToString();
  EXPECT_EQ(*contents, "block contents");
}

TEST_F(ReadBlockTest, RejectsEveryFlippedByte) {
  const std::uint64_t size = WriteBlockFile(path_, "block contents");
  std::string pristine;
  {
    auto file = Env::Default()->NewSequentialFile(path_);
    char scratch[64];
    pristine = std::string(*(*file)->Read(sizeof(scratch), scratch));
  }
  for (std::size_t offset = 0; offset < pristine.size(); ++offset) {
    std::string damaged = pristine;
    damaged[offset] ^= 0x10;
    {
      auto out = Env::Default()->NewWritableFile(path_);
      ASSERT_TRUE((*out)->Append(damaged).ok());
      ASSERT_TRUE((*out)->Close().ok());
    }
    const auto file = OpenFile();
    EXPECT_TRUE(strata::table::ReadBlock(*file, size, BlockHandle(0, 14)).status().IsCorruption())
        << "flipped byte " << offset;
  }
}

TEST_F(ReadBlockTest, RejectsUnknownCompressionEvenWithAValidChecksum) {
  const std::uint64_t size = WriteBlockFile(path_, "block contents", 7);
  const auto file = OpenFile();
  const auto status = strata::table::ReadBlock(*file, size, BlockHandle(0, 14)).status();
  EXPECT_TRUE(status.IsCorruption());
  EXPECT_NE(status.message().find("compression"), Slice::npos);
}

// A handle from a corrupt index can claim anything; the reader must reject
// one that leaves the file before allocating a byte for it.
TEST_F(ReadBlockTest, RejectsHandlesOutsideTheFile) {
  const std::uint64_t size = WriteBlockFile(path_, "block contents");
  const auto file = OpenFile();
  for (const BlockHandle& handle :
       {BlockHandle(0, size), BlockHandle(size, 0), BlockHandle(1, 14), BlockHandle(0, UINT64_MAX),
        BlockHandle(UINT64_MAX, 1), BlockHandle(UINT64_MAX - 2, 10)}) {
    EXPECT_TRUE(strata::table::ReadBlock(*file, size, handle).status().IsCorruption())
        << handle.offset() << "+" << handle.size();
  }
}

}  // namespace

#include "table/compression.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "support/string_file.hpp"
#include "table/format.hpp"
#include "util/coding.hpp"
#include "util/crc32c.hpp"

namespace strata::table {
namespace {

std::vector<CompressionType> BuiltIn() {
  std::vector<CompressionType> types;
  for (const CompressionType type : {CompressionType::kSnappy, CompressionType::kZstd}) {
    if (CompressorFor(type) != nullptr) {
      types.push_back(type);
    }
  }
  return types;
}

// Text-like: a small vocabulary repeated, as keys and values often are.
std::string Compressible(std::size_t size) {
  static const std::vector<std::string> kWords = {
      "user", "order", "2026-11-23", "status=ok", "region=eu-west", "amount", "pending"};
  std::mt19937 random(7);
  std::string text;
  while (text.size() < size) {
    text += kWords[random() % kWords.size()];
    text += ' ';
  }
  text.resize(size);
  return text;
}

std::string Random(std::size_t size) {
  std::mt19937 random(11);
  std::string bytes(size, '\0');
  for (char& byte : bytes) {
    byte = static_cast<char>(random() & 0xFF);
  }
  return bytes;
}

// A block file: contents, then a trailer with a valid checksum.
std::string BlockFile(Slice contents, CompressionType type) {
  std::string bytes(contents.data(), contents.size());
  bytes.push_back(static_cast<char>(type));
  PutFixed32(&bytes, crc32c::Mask(crc32c::Value(bytes)));
  return bytes;
}

Result<std::string> Read(const std::string& file, std::size_t contents_size) {
  const test::StringRandomAccessFile source(file);
  return ReadBlock(source, file.size(), BlockHandle(0, contents_size));
}

TEST(CompressionTest, NoneHasNoCompressor) {
  EXPECT_EQ(CompressorFor(CompressionType::kNone), nullptr);
  EXPECT_STREQ(CompressionName(CompressionType::kZstd), "zstd");
  EXPECT_STREQ(CompressionName(static_cast<CompressionType>(9)), "unknown");
}

TEST(CompressionTest, ThisBuildIncludesSnappyAndZstd) {
  // The default build has both; STRATA_WITH_* can leave either out.
  EXPECT_EQ(BuiltIn().size(), 2U);
}

TEST(CompressionTest, EveryAlgorithmRoundTrips) {
  for (const CompressionType type : BuiltIn()) {
    for (const std::size_t size : {0UL, 1UL, 4096UL, 300'000UL}) {
      const std::string raw = Compressible(size);
      std::string compressed;
      ASSERT_TRUE(CompressorFor(type)->Compress(raw, 0, &compressed));
      const Result<std::string> restored = CompressorFor(type)->Uncompress(compressed);
      ASSERT_TRUE(restored.ok()) << CompressionName(type) << ": " << restored.status().ToString();
      EXPECT_EQ(*restored, raw) << CompressionName(type) << " size " << size;
    }
  }
}

TEST(CompressionTest, CompressibleBlocksAreStoredCompressed) {
  const std::string raw = Compressible(4096);
  for (const CompressionType type : BuiltIn()) {
    std::string compressed;
    EXPECT_EQ(ChooseBlockEncoding(type, 0, raw, &compressed), type);
    EXPECT_LT(compressed.size(), raw.size() / 2) << CompressionName(type);
  }
  std::string unused;
  EXPECT_EQ(ChooseBlockEncoding(CompressionType::kNone, 0, raw, &unused), CompressionType::kNone);
}

TEST(CompressionTest, BlocksThatBarelyShrinkAreStoredRaw) {
  // Random bytes do not compress; storing them "compressed" would only cost
  // a decompression on every read.
  const std::string raw = Random(4096);
  for (const CompressionType type : BuiltIn()) {
    std::string compressed;
    EXPECT_EQ(ChooseBlockEncoding(type, 0, raw, &compressed), CompressionType::kNone)
        << CompressionName(type);
  }
}

TEST(CompressionTest, GarbageAndTruncatedInputIsCorruption) {
  for (const CompressionType type : BuiltIn()) {
    const Compressor* compressor = CompressorFor(type);
    EXPECT_TRUE(compressor->Uncompress("").status().IsCorruption()) << CompressionName(type);
    EXPECT_TRUE(compressor->Uncompress(Random(100)).status().IsCorruption())
        << CompressionName(type);
    std::string compressed;
    ASSERT_TRUE(compressor->Compress(Compressible(4096), 0, &compressed));
    compressed.resize(compressed.size() / 2);
    EXPECT_TRUE(compressor->Uncompress(compressed).status().IsCorruption())
        << CompressionName(type);
  }
}

// A header claiming more than the block limit is refused before anything is
// allocated for it: a damaged file cannot make the reader allocate gigabytes.
TEST(CompressionTest, AClaimedSizePastTheLimitIsCorruption) {
  if (const Compressor* snappy = CompressorFor(CompressionType::kSnappy)) {
    std::string claims_1gib;
    PutVarint32(&claims_1gib, 1U << 30);
    claims_1gib += "x";
    EXPECT_TRUE(snappy->Uncompress(claims_1gib).status().IsCorruption());
  }
  if (const Compressor* zstd = CompressorFor(CompressionType::kZstd)) {
    // Magic, then a frame header descriptor for a single segment with a
    // 4-byte content size, then that size: 1 GiB.
    std::string claims_1gib;
    PutFixed32(&claims_1gib, 0xFD2FB528U);
    claims_1gib.push_back(static_cast<char>(0xA0));
    PutFixed32(&claims_1gib, 1U << 30);
    EXPECT_TRUE(zstd->Uncompress(claims_1gib).status().IsCorruption());
  }
}

TEST(CompressionTest, ReadBlockUncompressesWhatTheTrailerNames) {
  const std::string raw = Compressible(4096);
  for (const CompressionType type : BuiltIn()) {
    std::string compressed;
    ASSERT_EQ(ChooseBlockEncoding(type, 0, raw, &compressed), type);
    const Result<std::string> read = Read(BlockFile(compressed, type), compressed.size());
    ASSERT_TRUE(read.ok()) << read.status().ToString();
    EXPECT_EQ(*read, raw);
  }
}

// The checksum covers the compressed bytes, so a damaged block never reaches
// the decompressor; one that does (a valid checksum over bad data) must still
// come back as Corruption, not a crash.
TEST(CompressionTest, ReadBlockReportsUndecompressableBlocksAsCorruption) {
  const std::string garbage = Random(64);
  for (const CompressionType type : BuiltIn()) {
    EXPECT_TRUE(Read(BlockFile(garbage, type), garbage.size()).status().IsCorruption())
        << CompressionName(type);
  }
}

}  // namespace
}  // namespace strata::table

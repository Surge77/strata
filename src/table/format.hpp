#pragma once

#include "strata/env.hpp"
#include "strata/result.hpp"
#include "strata/slice.hpp"
#include "strata/status.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

// The SSTable's framing: block handles, the footer, and checksummed block
// reads. The byte layout is specified in docs/format/sstable.md.

namespace strata::table {

// Where a block lives in the file.
class BlockHandle {
 public:
  // Two varint64s.
  static constexpr std::size_t kMaxEncodedLength = 10 + 10;

  BlockHandle() = default;
  BlockHandle(std::uint64_t offset, std::uint64_t size) : offset_(offset), size_(size) {}

  [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

  void EncodeTo(std::string* destination) const;

  // Consumes a handle from the front of *input. Corruption if malformed.
  Status DecodeFrom(Slice* input);

 private:
  std::uint64_t offset_ = 0;
  std::uint64_t size_ = 0;
};

// "STRATAST" read as a big-endian 64-bit number.
inline constexpr std::uint64_t kTableMagicNumber = 0x5354524154415354ULL;

// The fixed-size record at the end of every SSTable.
class Footer {
 public:
  // Two handles padded to 40 bytes, then the 8-byte magic number.
  static constexpr std::size_t kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

  BlockHandle metaindex_handle;
  BlockHandle index_handle;

  void EncodeTo(std::string* destination) const;
  // Corruption unless `input` is exactly one well-formed footer.
  Status DecodeFrom(Slice input);
};

enum class CompressionType : std::uint8_t {
  kNone = 0,
};

// compression type (1) + masked CRC-32C (4)
inline constexpr std::size_t kBlockTrailerSize = 5;

// Reads the block `handle` points to, verifies its trailer, and returns its
// contents (without the trailer). A block whose checksum or compression type
// is wrong, or which does not lie entirely inside a file of `file_size` bytes,
// is Corruption: its bytes are never handed to a parser.
//
// The size check happens before any allocation, so a corrupt handle claiming
// a multi-gigabyte block costs nothing -- it cannot be turned into a
// memory-exhaustion attack.
Result<std::string> ReadBlock(const RandomAccessFile& file, std::uint64_t file_size,
                              const BlockHandle& handle);

}  // namespace strata::table

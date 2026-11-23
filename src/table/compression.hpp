#pragma once

#include "strata/result.hpp"
#include "strata/slice.hpp"

#include <cstddef>
#include <string>

#include "table/format.hpp"

namespace strata::table {

// A block longer than this is always stored uncompressed, and a compressed
// block that says it expands past it is corrupt. Both formats carry the
// uncompressed length in their own header, which a damaged or crafted file
// can make up (and give a valid checksum), so this bounds what a few stored
// bytes can make the reader allocate and zero on every read. A data block is
// about Options::block_size (4 KiB by default); only a block holding a value
// of megabytes reaches it, and such a block is simply stored raw.
inline constexpr std::size_t kMaxUncompressedBlock = 4 * 1024 * 1024;

// Whether storing `compressed_size` bytes instead of `raw_size` is worth a
// decompression on every read: it must save at least an eighth (as in
// LevelDB).
[[nodiscard]] bool WorthCompressing(std::size_t raw_size, std::size_t compressed_size) noexcept;

// One compression algorithm. Adding another takes a CompressionType value
// (the byte in every block trailer), a subclass, and a case in CompressorFor.
class Compressor {
 public:
  Compressor() = default;
  Compressor(const Compressor&) = delete;
  Compressor& operator=(const Compressor&) = delete;
  Compressor(Compressor&&) = delete;
  Compressor& operator=(Compressor&&) = delete;
  virtual ~Compressor() = default;

  // Replaces `output` with the compressed form of `input`. `level` is the
  // algorithm's own knob (0 for its default) and may be ignored. False if the
  // input cannot be compressed, in which case the block is stored raw.
  virtual bool Compress(Slice input, int level, std::string* output) const = 0;

  // The original bytes, or Corruption. Never allocates more than
  // kMaxUncompressedBlock, whatever `input` claims.
  [[nodiscard]] virtual Result<std::string> Uncompress(Slice input) const = 0;
};

// The compressor for `type`, or null for kNone and for algorithms this build
// leaves out (see STRATA_WITH_SNAPPY and STRATA_WITH_ZSTD).
[[nodiscard]] const Compressor* CompressorFor(CompressionType type) noexcept;

// What to store for a block whose raw contents are `raw`: `preferred`, with
// the compressed bytes in `*compressed`, when that saves at least an eighth
// of the block; otherwise kNone, and the block goes in raw. Blocks above
// kMaxUncompressedBlock are never compressed.
[[nodiscard]] CompressionType ChooseBlockEncoding(CompressionType preferred, int level, Slice raw,
                                                  std::string* compressed);

// "none", "snappy", "zstd", or "unknown".
[[nodiscard]] const char* CompressionName(CompressionType type) noexcept;

}  // namespace strata::table

#include "table/compression.hpp"

#include <string>

#if STRATA_HAVE_SNAPPY
#include <snappy.h>
#endif
#if STRATA_HAVE_ZSTD
#include <zstd.h>
#endif

namespace strata::table {
namespace {

// Compressing has to win back at least this fraction of a block to be worth
// the work of uncompressing it on every read (LevelDB uses the same 1/8).
constexpr std::size_t kMinSavingDivisor = 8;

#if STRATA_HAVE_SNAPPY
// The raw snappy format (not its framing format): a varint32 length, then the
// compressed stream. snappy has no checksum; the block trailer's CRC covers
// the compressed bytes.
class SnappyCompressor final : public Compressor {
 public:
  bool Compress(Slice input, int /*level*/, std::string* output) const override {
    output->resize(snappy::MaxCompressedLength(input.size()));
    std::size_t length = 0;
    snappy::RawCompress(input.data(), input.size(), output->data(), &length);
    output->resize(length);
    return true;
  }

  Result<std::string> Uncompress(Slice input) const override {
    std::size_t length = 0;
    if (!snappy::GetUncompressedLength(input.data(), input.size(), &length)) {
      return Status::Corruption("snappy block has no valid length");
    }
    // The writer never compresses an empty block.
    if (length == 0 || length > kMaxUncompressedBlock) {
      return Status::Corruption("snappy block claims an impossible length");
    }
    // RawUncompress writes exactly `length` bytes or fails; never the
    // string-resizing Uncompress, which would allocate whatever the header says.
    std::string output(length, '\0');
    if (!snappy::RawUncompress(input.data(), input.size(), output.data())) {
      return Status::Corruption("snappy block does not decompress");
    }
    return output;
  }
};
#endif

#if STRATA_HAVE_ZSTD
// One context per thread, kept for the thread's life: ZSTD_compress and
// ZSTD_decompress would create and free one for every block. A context holds
// no state between calls that could leak one block into the next.
struct ZstdContexts {
  ZSTD_CCtx* compress = ZSTD_createCCtx();
  ZSTD_DCtx* decompress = ZSTD_createDCtx();
  ZstdContexts() = default;
  ZstdContexts(const ZstdContexts&) = delete;
  ZstdContexts& operator=(const ZstdContexts&) = delete;
  ZstdContexts(ZstdContexts&&) = delete;
  ZstdContexts& operator=(ZstdContexts&&) = delete;
  ~ZstdContexts() {
    ZSTD_freeCCtx(compress);
    ZSTD_freeDCtx(decompress);
  }
};

ZstdContexts& ThreadContexts() {
  thread_local ZstdContexts contexts;
  return contexts;
}
ZSTD_CCtx* ThreadCompressContext() {
  return ThreadContexts().compress;
}
ZSTD_DCtx* ThreadDecompressContext() {
  return ThreadContexts().decompress;
}

// One zstd frame that records its content size (ZSTD_compressCCtx always does).
class ZstdCompressor final : public Compressor {
 public:
  bool Compress(Slice input, int level, std::string* output) const override {
    if (ThreadCompressContext() == nullptr) {
      return false;  // out of memory: store the block raw
    }
    output->resize(ZSTD_compressBound(input.size()));
    const std::size_t length = ZSTD_compressCCtx(ThreadCompressContext(), output->data(),
                                                 output->size(), input.data(), input.size(), level);
    if (ZSTD_isError(length) != 0U) {
      return false;
    }
    output->resize(length);
    return true;
  }

  Result<std::string> Uncompress(Slice input) const override {
    const unsigned long long claimed = ZSTD_getFrameContentSize(input.data(), input.size());
    if (claimed == ZSTD_CONTENTSIZE_ERROR || claimed == ZSTD_CONTENTSIZE_UNKNOWN) {
      return Status::Corruption("zstd block has no valid frame header");
    }
    // 0 also covers a skippable frame, which the writer never produces.
    if (claimed == 0 || claimed > kMaxUncompressedBlock) {
      return Status::Corruption("zstd block claims an impossible length");
    }
    if (ThreadDecompressContext() == nullptr) {
      return Status::IoError("out of memory for a zstd decompression context");
    }
    const auto length = static_cast<std::size_t>(claimed);
    std::string output(length, '\0');
    // Bounded by `length`: zstd never writes past the capacity it is given, and
    // a second frame or a lying header shows up as an error or a short result.
    const std::size_t written = ZSTD_decompressDCtx(ThreadDecompressContext(), output.data(),
                                                    output.size(), input.data(), input.size());
    if (ZSTD_isError(written) != 0U || written != length) {
      return Status::Corruption("zstd block does not decompress");
    }
    return output;
  }
};
#endif

}  // namespace

const Compressor* CompressorFor(CompressionType type) noexcept {
  switch (type) {
    case CompressionType::kNone:
      return nullptr;
    case CompressionType::kSnappy: {
#if STRATA_HAVE_SNAPPY
      static const SnappyCompressor snappy;
      return &snappy;
#else
      return nullptr;
#endif
    }
    case CompressionType::kZstd: {
#if STRATA_HAVE_ZSTD
      static const ZstdCompressor zstd;
      return &zstd;
#else
      return nullptr;
#endif
    }
  }
  return nullptr;
}

CompressionType ChooseBlockEncoding(CompressionType preferred, int level, Slice raw,
                                    std::string* compressed) {
  const Compressor* compressor = CompressorFor(preferred);
  if (compressor == nullptr || raw.size() > kMaxUncompressedBlock ||
      !compressor->Compress(raw, level, compressed)) {
    return CompressionType::kNone;
  }
  return WorthCompressing(raw.size(), compressed->size()) ? preferred : CompressionType::kNone;
}

bool WorthCompressing(std::size_t raw_size, std::size_t compressed_size) noexcept {
  return compressed_size < raw_size - raw_size / kMinSavingDivisor;
}

const char* CompressionName(CompressionType type) noexcept {
  switch (type) {
    case CompressionType::kNone:
      return "none";
    case CompressionType::kSnappy:
      return "snappy";
    case CompressionType::kZstd:
      return "zstd";
  }
  return "unknown";
}

}  // namespace strata::table

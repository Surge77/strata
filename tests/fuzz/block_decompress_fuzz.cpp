// Fuzz target: hand arbitrary bytes straight to each decompressor, past the
// block checksum that normally stands in front of them. The first byte picks
// the algorithm. Checks memory safety (ASan/libFuzzer, with snappy and zstd
// themselves instrumented), that nothing longer than the block limit is ever
// produced, and that whatever decompresses re-compresses to the same bytes.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "table/compression.hpp"

namespace {

using strata::table::CompressionType;
using strata::table::Compressor;
using strata::table::CompressorFor;

std::string Text(std::size_t size) {
  std::string text;
  while (text.size() < size) {
    text += "key=value; another key=another value; ";
  }
  text.resize(size);
  return text;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  const auto type = (data[0] & 1) == 0 ? CompressionType::kSnappy : CompressionType::kZstd;
  const Compressor* compressor = CompressorFor(type);
  if (compressor == nullptr) {
    return 0;
  }
  const strata::Slice input(reinterpret_cast<const char*>(data + 1), size - 1);
  const strata::Result<std::string> output = compressor->Uncompress(input);
  if (!output.ok()) {
    return 0;
  }
  if (output->size() > strata::table::kMaxUncompressedBlock) {
    std::abort();
  }
  std::string again;
  if (compressor->Compress(*output, 0, &again)) {
    const strata::Result<std::string> round_trip = compressor->Uncompress(again);
    if (!round_trip.ok() || *round_trip != *output) {
      std::abort();
    }
  }
  return 0;
}

std::vector<std::string> StrataFuzzSeeds() {
  std::vector<std::string> seeds;
  for (const auto& [selector, type] :
       {std::pair{'\0', CompressionType::kSnappy}, std::pair{'\1', CompressionType::kZstd}}) {
    const Compressor* compressor = CompressorFor(type);
    if (compressor == nullptr) {
      continue;
    }
    for (const std::size_t size : {0UL, 17UL, 4096UL}) {
      std::string compressed;
      if (compressor->Compress(Text(size), 0, &compressed)) {
        seeds.push_back(std::string(1, selector) + compressed);
      }
    }
  }
  return seeds;
}

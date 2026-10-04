#include "table/bloom_filter.hpp"

#include <algorithm>
#include <cstddef>

#include "db/dbformat.hpp"

namespace strata::table {
namespace {

constexpr std::uint8_t kProbes = 7;
constexpr std::size_t kBitsPerKey = 10;
constexpr std::size_t kMinBits = 64;

std::size_t BitPosition(std::uint64_t hash, std::uint8_t probe, std::size_t bit_count) {
  const auto first = static_cast<std::uint32_t>(hash);
  const auto step = static_cast<std::uint32_t>(hash >> 32U) | 1U;
  const std::uint32_t combined = first + static_cast<std::uint32_t>(probe) * step;
  return combined % bit_count;
}

}  // namespace

std::uint64_t BloomFilter::Hash(Slice key) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char byte : key) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= 1099511628211ULL;
  }
  hash ^= hash >> 30U;
  hash *= 0xBF58476D1CE4E5B9ULL;
  hash ^= hash >> 27U;
  hash *= 0x94D049BB133111EBULL;
  return hash ^ (hash >> 31U);
}

std::string BloomFilter::Build(std::span<const std::uint64_t> hashes) {
  const std::size_t bits = std::max(kMinBits, hashes.size() * kBitsPerKey);
  const std::size_t bytes = ((bits + 63U) / 64U) * 8U;
  std::string encoded(1 + bytes, '\0');
  encoded[0] = static_cast<char>(kProbes);
  const std::size_t bit_count = bytes * 8U;
  for (const std::uint64_t hash : hashes) {
    for (std::uint8_t probe = 0; probe < kProbes; ++probe) {
      const std::size_t bit = BitPosition(hash, probe, bit_count);
      encoded[1 + bit / 8U] =
          static_cast<char>(static_cast<std::uint8_t>(encoded[1 + bit / 8U]) | (1U << (bit % 8U)));
    }
  }
  return encoded;
}

Status BloomFilter::Validate(Slice encoded) {
  if (encoded.size() < 9 || (encoded.size() - 1) % 8 != 0 ||
      static_cast<std::uint8_t>(encoded[0]) != kProbes) {
    return Status::Corruption("invalid table Bloom filter");
  }
  return Status::Ok();
}

bool BloomFilter::SupportsComparator(const Comparator* comparator) noexcept {
  const auto* internal = dynamic_cast<const InternalKeyComparator*>(comparator);
  return internal != nullptr && internal->user_comparator() == BytewiseComparator();
}

bool BloomFilter::MayContain(Slice key, Slice encoded) noexcept {
  const std::uint64_t hash = Hash(key);
  const std::size_t bit_count = (encoded.size() - 1) * 8U;
  for (std::uint8_t probe = 0; probe < kProbes; ++probe) {
    const std::size_t bit = BitPosition(hash, probe, bit_count);
    if ((static_cast<std::uint8_t>(encoded[1 + bit / 8U]) & (1U << (bit % 8U))) == 0) {
      return false;
    }
  }
  return true;
}

}  // namespace strata::table

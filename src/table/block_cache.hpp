#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "table/block.hpp"
#include "util/lru_cache.hpp"

namespace strata::table {

// Names one data block among all the tables sharing a cache: the owning
// table's cache id plus the block's file offset. The database uses table file
// numbers as ids; they are never reused within a process, so a block cached
// from a deleted table can never be served for a newer one.
struct BlockCacheKey {
  std::uint64_t cache_id = 0;
  std::uint64_t offset = 0;

  friend bool operator==(const BlockCacheKey&, const BlockCacheKey&) = default;
};

struct BlockCacheKeyHash {
  std::size_t operator()(const BlockCacheKey& key) const noexcept {
    // Mixing the id by the golden-ratio constant keeps blocks at the same
    // offset in different tables from landing in the same bucket.
    constexpr std::uint64_t kGoldenRatio = 0x9E3779B97F4A7C15ULL;
    return std::hash<std::uint64_t>{}((key.cache_id * kGoldenRatio) ^ key.offset);
  }
};

// Parsed data blocks, charged by their size in bytes.
using BlockCache = LruCache<BlockCacheKey, Block, BlockCacheKeyHash>;

}  // namespace strata::table

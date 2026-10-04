#pragma once

#include "strata/slice.hpp"
#include "strata/status.hpp"

#include <cstdint>
#include <span>
#include <string>

#include "util/comparator.hpp"

namespace strata::table {

inline constexpr Slice kUserBloomFilterMetaName = "filter.strata.user-bloom.v1";

// A whole-table Bloom filter over user keys (docs/format/bloom_filter.md).
class BloomFilter {
 public:
  [[nodiscard]] static std::uint64_t Hash(Slice key) noexcept;
  [[nodiscard]] static std::string Build(std::span<const std::uint64_t> hashes);
  [[nodiscard]] static Status Validate(Slice encoded);
  [[nodiscard]] static bool SupportsComparator(const Comparator* comparator) noexcept;
  // Precondition: Validate(encoded).ok().
  [[nodiscard]] static bool MayContain(Slice key, Slice encoded) noexcept;
};

}  // namespace strata::table

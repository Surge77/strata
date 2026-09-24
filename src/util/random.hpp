#pragma once

#include <cstdint>

namespace strata {

// A small, fast, deterministic pseudo-random generator (SplitMix64).
//
// Used where the engine needs randomness that is cheap and reproducible rather
// than cryptographic: skiplist node heights, and test workloads. Seeding it
// the same way gives the same sequence on every platform, which keeps
// skiplist-shape-dependent bugs reproducible from a seed in a failing test.
class Random {
 public:
  explicit Random(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t Next() noexcept {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

  // Uniform in [0, n). Precondition: n > 0. The modulo bias is below 2^-40 for
  // the small n the engine uses, which is irrelevant to skiplist balance.
  std::uint64_t Uniform(std::uint64_t n) noexcept { return Next() % n; }

  // True with probability 1/n. Precondition: n > 0.
  bool OneIn(std::uint64_t n) noexcept { return Uniform(n) == 0; }

 private:
  std::uint64_t state_;
};

}  // namespace strata

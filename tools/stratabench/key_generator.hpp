#pragma once

#include <cstdint>

#include "util/random.hpp"

namespace strata::bench {

// Uniform double in [0, 1) from the top 53 bits of a 64-bit draw.
[[nodiscard]] inline double UniformDouble(Random* random) noexcept {
  constexpr double kTwoToMinus53 = 0x1.0p-53;
  constexpr unsigned kDiscardedBits = 11;
  return static_cast<double>(random->Next() >> kDiscardedBits) * kTwoToMinus53;
}

// Zipf-distributed ranks in [0, items): rank r has probability proportional to
// 1 / (r + 1)^theta. The method is Gray et al., "Quickly Generating
// Billion-Record Synthetic Databases" (SIGMOD 1994), as used by YCSB. Setup
// sums zeta(items) once, O(items); each draw is O(1).
class ZipfianGenerator {
 public:
  static constexpr double kDefaultTheta = 0.99;  // YCSB's default skew

  // Preconditions: 1 <= items < kScramblePrime, 0 < theta < 1.
  ZipfianGenerator(std::uint64_t items, double theta, std::uint64_t seed);

  [[nodiscard]] std::uint64_t NextRank() noexcept { return RankFor(UniformDouble(&random_)); }

  // The rank for a uniform draw `u` in [0, 1). Const, so one generator can
  // serve many threads that bring their own draws.
  [[nodiscard]] std::uint64_t RankFor(double u) const noexcept;

  // Maps a rank onto the key space by a fixed permutation, so the popular
  // ranks become keys spread across the whole range rather than the first few
  // keys -- which would all sit in one table. Multiplication by a prime that
  // does not divide `items` is a bijection on [0, items), so the hot set keeps
  // exactly the size Zipf gives it (a hash would merge some ranks).
  [[nodiscard]] static std::uint64_t Scramble(std::uint64_t rank, std::uint64_t items) noexcept;

  // The largest prime below 2^32: (rank + 1) * prime fits in 64 bits.
  static constexpr std::uint64_t kScramblePrime = 4'294'967'291ULL;

 private:
  std::uint64_t items_;
  double zeta_items_;
  double alpha_;
  double eta_;
  double half_pow_theta_;  // 0.5^theta: the probability boundary of rank 1
  Random random_;
};

enum class KeyDistribution { kUniform, kZipfian };

// Chooses key indexes in [0, items) under the given distribution. Zipfian
// choices are scrambled (see ZipfianGenerator::Scramble). Not thread-safe:
// give each thread its own, seeded differently.
class KeyChooser {
 public:
  KeyChooser(KeyDistribution distribution, std::uint64_t items, std::uint64_t seed);

  [[nodiscard]] std::uint64_t Next() noexcept;

 private:
  KeyDistribution distribution_;
  std::uint64_t items_;
  Random random_;
  ZipfianGenerator zipf_;
};

}  // namespace strata::bench

#include "stratabench/key_generator.hpp"

#include <algorithm>
#include <cmath>

namespace strata::bench {
namespace {

double Zeta(std::uint64_t items, double theta) {
  double sum = 0;
  for (std::uint64_t rank = 1; rank <= items; ++rank) {
    sum += 1.0 / std::pow(static_cast<double>(rank), theta);
  }
  return sum;
}

// A Zipfian chooser over one item would divide by zero in eta; it is never
// drawn from (Next returns 0), so any valid size works for it.
constexpr std::uint64_t kMinZipfItems = 2;

}  // namespace

ZipfianGenerator::ZipfianGenerator(std::uint64_t items, double theta, std::uint64_t seed)
    : items_(std::max(items, kMinZipfItems)),
      zeta_items_(Zeta(items_, theta)),
      alpha_(1.0 / (1.0 - theta)),
      eta_((1.0 - std::pow(2.0 / static_cast<double>(items_), 1.0 - theta)) /
           (1.0 - Zeta(2, theta) / zeta_items_)),
      half_pow_theta_(std::pow(0.5, theta)),
      random_(seed) {}

std::uint64_t ZipfianGenerator::RankFor(double u) const noexcept {
  const double uz = u * zeta_items_;
  if (uz < 1.0) {
    return 0;
  }
  if (uz < 1.0 + half_pow_theta_) {
    return 1;
  }
  const double rank = static_cast<double>(items_) * std::pow(eta_ * u - eta_ + 1.0, alpha_);
  return std::min(static_cast<std::uint64_t>(rank), items_ - 1);
}

std::uint64_t ZipfianGenerator::Scramble(std::uint64_t rank, std::uint64_t items) noexcept {
  return ((rank + 1) * kScramblePrime) % items;
}

KeyChooser::KeyChooser(KeyDistribution distribution, std::uint64_t items, std::uint64_t seed)
    : distribution_(distribution),
      items_(items),
      random_(seed),
      zipf_(items, ZipfianGenerator::kDefaultTheta, seed ^ 0x5DEECE66DULL) {}

std::uint64_t KeyChooser::Next() noexcept {
  if (items_ <= 1) {
    return 0;
  }
  if (distribution_ == KeyDistribution::kUniform) {
    return random_.Uniform(items_);
  }
  return ZipfianGenerator::Scramble(zipf_.NextRank(), items_);
}

}  // namespace strata::bench

#include "util/comparator.hpp"

#include <algorithm>
#include <cstdint>

namespace strata {
namespace {

class BytewiseComparatorImpl final : public Comparator {
 public:
  int Compare(Slice a, Slice b) const override { return a.compare(b); }

  std::string_view Name() const override { return "strata.BytewiseComparator"; }

  // Keeps the common prefix and, if possible, bumps the first differing byte:
  // start="abcxyz", limit="abezz" gives "abd".
  void FindShortestSeparator(std::string* start, Slice limit) const override {
    const std::size_t min_length = std::min(start->size(), limit.size());
    std::size_t diff = 0;
    while (diff < min_length && (*start)[diff] == limit[diff]) {
      ++diff;
    }
    if (diff >= min_length) {
      return;  // one is a prefix of the other: no shorter key fits between them
    }
    const auto byte = static_cast<std::uint8_t>((*start)[diff]);
    if (byte < 0xFFU && byte + 1U < static_cast<std::uint8_t>(limit[diff])) {
      (*start)[diff] = static_cast<char>(byte + 1U);
      start->resize(diff + 1);
    }
  }

  // Bumps the first byte that is not 0xFF and drops the rest: "abc" -> "b".
  // A key of all 0xFF bytes has no shorter successor and is left alone.
  void FindShortSuccessor(std::string* key) const override {
    for (std::size_t index = 0; index < key->size(); ++index) {
      const auto byte = static_cast<std::uint8_t>((*key)[index]);
      if (byte != 0xFFU) {
        (*key)[index] = static_cast<char>(byte + 1U);
        key->resize(index + 1);
        return;
      }
    }
  }
};

}  // namespace

const Comparator* BytewiseComparator() {
  static const BytewiseComparatorImpl* const kInstance = new BytewiseComparatorImpl();
  return kInstance;
}

}  // namespace strata

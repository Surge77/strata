#include "stratabench/stats.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace strata::bench {

Summary Summarize(std::span<const double> values) {
  if (values.empty()) {
    return {};
  }
  std::vector<double> sorted(values.begin(), values.end());
  std::sort(sorted.begin(), sorted.end());
  const std::size_t count = sorted.size();
  const std::size_t middle = count / 2;
  const double median =
      count % 2 == 1 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2.0;
  const double mean =
      std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(count);

  double cv = 0;
  if (count > 1 && mean != 0) {
    double squared_deviations = 0;
    for (const double value : sorted) {
      squared_deviations += (value - mean) * (value - mean);
    }
    cv = std::sqrt(squared_deviations / static_cast<double>(count - 1)) / std::abs(mean);
  }
  return {.median = median,
          .mean = mean,
          .min = sorted.front(),
          .max = sorted.back(),
          .cv = cv,
          .rounds = count};
}

}  // namespace strata::bench

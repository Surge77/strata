#include "stratabench/latency.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>

#include "stratabench/open_loop.hpp"

namespace strata::bench {
namespace {

constexpr double kNanosPerMicro = 1e3;
constexpr double kP50 = 50;
constexpr double kP99 = 99;
constexpr double kP999 = 99.9;
// Keeps open-loop round seeds apart from the closed-loop ones.
constexpr std::uint64_t kOpenLoopSeedOffset = 0x0E1;

double Micros(double nanos) {
  return nanos / kNanosPerMicro;
}

// Per-round samples of every reported metric, summarised at the end.
struct RowSamples {
  std::vector<double> achieved, p50, p99, p999, max, service_p50, service_p99;
  int saturated = 0;

  void Add(const OpenLoopResult& result) {
    achieved.push_back(result.achieved_rate);
    p50.push_back(Micros(result.response_ns.Percentile(kP50)));
    p99.push_back(Micros(result.response_ns.Percentile(kP99)));
    p999.push_back(Micros(result.response_ns.Percentile(kP999)));
    max.push_back(Micros(static_cast<double>(result.response_ns.Max())));
    service_p50.push_back(Micros(result.service_ns.Percentile(kP50)));
    service_p99.push_back(Micros(result.service_ns.Percentile(kP99)));
    saturated += result.saturated ? 1 : 0;
  }

  [[nodiscard]] LatencyRow Summarise(double offered) const {
    return {.offered_rate = offered,
            .achieved_rate = Summarize(achieved),
            .response_p50 = Summarize(p50),
            .response_p99 = Summarize(p99),
            .response_p999 = Summarize(p999),
            .response_max = Summarize(max),
            .service_p50 = Summarize(service_p50),
            .service_p99 = Summarize(service_p99),
            .saturated_rounds = saturated};
  }
};

using OpFactory = std::function<std::function<void(std::uint64_t)>(int round)>;

LatencyRow MeasureRate(const BenchConfig& config, const LatencyConfig& latency, LoadedDb* loaded,
                       double rate, bool writes, const OpFactory& make_op) {
  RowSamples samples;
  const int total_rounds = config.protocol.warmup + config.protocol.rounds;
  for (int round = 0; round < total_rounds; ++round) {
    loaded->PrepareRound(config.cold, writes);
    std::fprintf(stderr, "  open loop %.0f op/s, round %d/%d\n", rate, round + 1, total_rounds);
    const OpenLoopConfig open{
        .rate = rate,
        .duration = latency.duration,
        .workers = latency.workers,
        .seed = config.seed + kOpenLoopSeedOffset + static_cast<std::uint64_t>(round)};
    const OpenLoopResult result = RunOpenLoop(open, make_op(round));
    if (round >= config.protocol.warmup) {
      samples.Add(result);
    }
  }
  return samples.Summarise(rate);
}

Summary ClosedLoopMax(const BenchConfig& config, const LatencyConfig& latency, LoadedDb* loaded) {
  int round = 0;
  const std::vector<double> values = RunRounds(config.protocol, [&] {
    return MixedThroughput(config, loaded, latency.read_fraction, latency.workers, round++);
  });
  return Summarize(values);
}

}  // namespace

LatencyReport RunLatency(const BenchConfig& config, const LatencyConfig& latency,
                         LoadedDb* loaded) {
  LatencyReport report{.ran = true,
                       .read_fraction = latency.read_fraction,
                       .workers = latency.workers,
                       .duration_seconds = std::chrono::duration<double>(latency.duration).count()};
  std::fprintf(stderr, "latency: closed-loop maximum\n");
  report.closed_loop_max = ClosedLoopMax(config, latency, loaded);

  std::vector<double> rates = latency.rates;
  if (rates.empty()) {
    for (const double fraction : kAutoRateFractions) {
      rates.push_back(fraction * report.closed_loop_max.median);
    }
  }
  // MixedOp sums zeta(keys) when built, so it is built once per round, before
  // the round's clock starts, and captured by the operation.
  const bool writes = latency.read_fraction < 1.0;
  std::vector<std::unique_ptr<MixedOp>> ops;
  const OpFactory strata_op = [&](int round) -> std::function<void(std::uint64_t)> {
    ops.push_back(std::make_unique<MixedOp>(
        loaded->db(), loaded->keys(), latency.read_fraction, config.value_size,
        config.seed + kOpenLoopSeedOffset + static_cast<std::uint64_t>(round)));
    const MixedOp* op = ops.back().get();
    return [op](std::uint64_t index) { (*op)(index); };
  };
  for (const double rate : rates) {
    report.rows.push_back(MeasureRate(config, latency, loaded, rate, writes, strata_op));
    ops.clear();
  }

  std::fprintf(stderr, "latency: harness floor (no-op)\n");
  const double highest = rates.empty() ? 0 : *std::max_element(rates.begin(), rates.end());
  const OpFactory no_op = [](int) { return [](std::uint64_t) {}; };
  report.harness_floor = MeasureRate(config, latency, loaded, highest, /*writes=*/false, no_op);
  return report;
}

}  // namespace strata::bench

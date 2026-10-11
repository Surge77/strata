#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "stratabench/env_info.hpp"
#include "stratabench/latency.hpp"
#include "stratabench/workloads.hpp"

namespace strata::bench {

struct Report {
  EnvInfo env{};
  BenchConfig config{};
  std::string db_shape{};  // tables per level of the loaded database, if built
  std::vector<WorkloadResult> workloads{};
  LatencyReport latency{};
};

// Every gated figure is stable (docs/benchmarks.md, "What is gated"):
// closed-loop throughput, and the achieved rate and median response time of
// each open-loop rate that did not saturate.
[[nodiscard]] bool AllGatedStable(const Report& report);

// Whether the run may be published at all: enough rounds, an optimised build.
[[nodiscard]] bool Publishable(const Report& report);

void PrintReport(const Report& report, std::FILE* out);

// Writes the report as JSON. Returns false if the file cannot be written.
[[nodiscard]] bool WriteJson(const Report& report, const std::filesystem::path& path);

// `text` as a JSON string literal, quotes included.
[[nodiscard]] std::string JsonString(std::string_view text);

}  // namespace strata::bench

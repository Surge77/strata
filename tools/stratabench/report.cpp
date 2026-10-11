#include "stratabench/report.hpp"

#include <format>
#include <fstream>

namespace strata::bench {
namespace {

constexpr double kPercent = 100.0;
constexpr unsigned char kFirstPrintableByte = 0x20;

std::string Flag(const Summary& summary) {
  return summary.stable() ? "" : "  UNSTABLE";
}

std::string SummaryJson(const Summary& s) {
  return std::format(
      R"({{"median": {}, "mean": {}, "min": {}, "max": {}, "cv": {}, "rounds": {}, "stable": {}}})",
      s.median, s.mean, s.min, s.max, s.cv, s.rounds, s.stable());
}

std::string ValuesJson(const std::vector<double>& values) {
  std::string out = "[";
  for (std::size_t index = 0; index < values.size(); ++index) {
    out += std::format("{}{}", index == 0 ? "" : ", ", values[index]);
  }
  return out + "]";
}

std::string RowJson(const LatencyRow& row) {
  return std::format(
      R"({{"offered_rate": {}, "achieved_rate": {}, "response_p50_us": {}, )"
      R"("response_p99_us": {}, "response_p999_us": {}, "response_max_us": {}, )"
      R"("service_p50_us": {}, "service_p99_us": {}, "saturated_rounds": {}}})",
      row.offered_rate, SummaryJson(row.achieved_rate), SummaryJson(row.response_p50),
      SummaryJson(row.response_p99), SummaryJson(row.response_p999), SummaryJson(row.response_max),
      SummaryJson(row.service_p50), SummaryJson(row.service_p99), row.saturated_rounds);
}

bool RowGatedStable(const LatencyRow& row) {
  return row.saturated_rounds > 0 || (row.achieved_rate.stable() && row.response_p50.stable());
}

void PrintRow(std::FILE* out, const char* label, const LatencyRow& row) {
  std::fprintf(out, "%-10s %10.0f %10.0f %5.1f%% %9.1f %5.1f%% %9.1f %9.1f %9.1f %9.1f%s%s\n",
               label, row.offered_rate, row.achieved_rate.median, kPercent * row.achieved_rate.cv,
               row.response_p50.median, kPercent * row.response_p50.cv, row.response_p99.median,
               row.response_p999.median, row.response_max.median, row.service_p99.median,
               row.saturated_rounds > 0 ? "  SATURATED" : "",
               RowGatedStable(row) ? "" : "  UNSTABLE");
}

}  // namespace

std::string JsonString(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (byte < kFirstPrintableByte) {
      out += std::format("\\u{:04x}", static_cast<unsigned>(byte));
    } else {
      out += c;
    }
  }
  return out + "\"";
}

bool AllGatedStable(const Report& report) {
  for (const auto& workload : report.workloads) {
    if (!workload.summary.stable()) {
      return false;
    }
  }
  if (!report.latency.ran) {
    return true;
  }
  if (!report.latency.closed_loop_max.stable()) {
    return false;
  }
  for (const auto& row : report.latency.rows) {
    if (!RowGatedStable(row)) {
      return false;
    }
  }
  return true;
}

bool Publishable(const Report& report) {
  return report.config.protocol.publishable() && report.env.publishable_build() &&
         !report.env.on_9p();
}

void PrintReport(const Report& report, std::FILE* out) {
  const EnvInfo& env = report.env;
  const BenchConfig& config = report.config;
  std::fprintf(out, "stratabench  %s  commit %s\n", env.timestamp_utc.c_str(), env.commit.c_str());
  std::fprintf(out, "cpu         %s (%u logical)%s\n", env.cpu_model.c_str(), env.logical_cpus,
               env.wsl ? "  [WSL2]" : "");
  std::fprintf(out, "memory      %llu MiB    kernel %s\n",
               static_cast<unsigned long long>(env.memory_mib), env.kernel.c_str());
  std::fprintf(out, "build       %s %s, sanitizer %s    scratch fs %s\n", env.compiler.c_str(),
               env.build_type.c_str(), env.sanitizer.c_str(), env.filesystem.c_str());
  std::fprintf(out, "config      num=%llu value=%zuB threads=%d reads=%llu cache=%s\n",
               static_cast<unsigned long long>(config.num), config.value_size, config.threads,
               static_cast<unsigned long long>(config.reads_per_round()),
               config.cold ? "cold" : "warm");
  std::fprintf(out, "protocol    warmup=%d rounds=%d, median and CV, stable at CV <= %.0f%%%s\n",
               config.protocol.warmup, config.protocol.rounds, kPercent * kMaxStableCv,
               Publishable(report) ? "" : "    NOT PUBLISHABLE");
  if (!report.db_shape.empty()) {
    std::fprintf(out, "loaded db   tables per level: %s\n", report.db_shape.c_str());
  }
  if (!report.workloads.empty()) {
    std::fprintf(out, "\n%-12s %14s %-10s %7s %14s %14s\n", "workload", "median", "unit", "CV",
                 "min", "max");
  }
  for (const auto& w : report.workloads) {
    std::fprintf(out, "%-12s %14.0f %-10s %6.1f%% %14.0f %14.0f%s\n", w.name.c_str(),
                 w.summary.median, w.unit.c_str(), kPercent * w.summary.cv, w.summary.min,
                 w.summary.max, Flag(w.summary).c_str());
  }
  const LatencyReport& latency = report.latency;
  if (latency.ran) {
    std::fprintf(out, "\nopen loop: %.0f%% reads, %d workers, %.1f s per round\n",
                 kPercent * latency.read_fraction, latency.workers, latency.duration_seconds);
    std::fprintf(out, "closed-loop max %.0f ops/s (CV %.1f%%)%s\n", latency.closed_loop_max.median,
                 kPercent * latency.closed_loop_max.cv, Flag(latency.closed_loop_max).c_str());
    std::fprintf(out, "%-10s %10s %10s %6s %9s %6s %9s %9s %9s %9s   (latency in us)\n", "",
                 "offered", "achieved", "CV", "p50", "CV", "p99", "p99.9", "max", "svc p99");
    for (const auto& row : latency.rows) {
      PrintRow(out, "strata", row);
    }
    PrintRow(out, "no-op", latency.harness_floor);
  }
}

bool WriteJson(const Report& report, const std::filesystem::path& path) {
  const EnvInfo& env = report.env;
  const BenchConfig& config = report.config;
  std::ofstream out(path);
  out << "{\n  \"env\": {"
      << std::format(R"("timestamp_utc": {}, "commit": {}, "cpu_model": {}, "logical_cpus": {}, )",
                     JsonString(env.timestamp_utc), JsonString(env.commit),
                     JsonString(env.cpu_model), env.logical_cpus)
      << std::format(R"("memory_mib": {}, "kernel": {}, "wsl": {}, "compiler": {}, )",
                     env.memory_mib, JsonString(env.kernel), env.wsl, JsonString(env.compiler))
      << std::format(R"("build_type": {}, "sanitizer": {}, "filesystem": {}}},)",
                     JsonString(env.build_type), JsonString(env.sanitizer),
                     JsonString(env.filesystem))
      << "\n  \"config\": "
      << std::format(R"({{"num": {}, "value_size": {}, "threads": {}, "reads": {}, "cold": {}, )",
                     config.num, config.value_size, config.threads, config.reads_per_round(),
                     config.cold)
      << std::format(R"("warmup": {}, "rounds": {}, "seed": {}, "max_stable_cv": {}}},)",
                     config.protocol.warmup, config.protocol.rounds, config.seed, kMaxStableCv)
      << std::format("\n  \"publishable\": {},\n  \"all_gated_stable\": {},", Publishable(report),
                     AllGatedStable(report))
      << "\n  \"loaded_db_shape\": " << JsonString(report.db_shape) << ",\n  \"workloads\": [";
  for (std::size_t index = 0; index < report.workloads.size(); ++index) {
    const WorkloadResult& w = report.workloads[index];
    out << (index == 0 ? "\n    " : ",\n    ")
        << std::format(R"({{"name": {}, "unit": {}, "values": {}, "summary": {}}})",
                       JsonString(w.name), JsonString(w.unit), ValuesJson(w.values),
                       SummaryJson(w.summary));
  }
  out << "\n  ]";
  const LatencyReport& latency = report.latency;
  if (latency.ran) {
    out << ",\n  \"latency\": "
        << std::format(R"({{"read_fraction": {}, "workers": {}, "duration_s": {}, )",
                       latency.read_fraction, latency.workers, latency.duration_seconds)
        << R"("closed_loop_max": )" << SummaryJson(latency.closed_loop_max) << ", \"rows\": [";
    for (std::size_t index = 0; index < latency.rows.size(); ++index) {
      out << (index == 0 ? "\n    " : ",\n    ") << RowJson(latency.rows[index]);
    }
    out << "\n  ], \"harness_floor\": " << RowJson(latency.harness_floor) << "}";
  }
  out << "\n}\n";
  return static_cast<bool>(out.flush());
}

}  // namespace strata::bench

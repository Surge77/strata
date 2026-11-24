#include "monitor.hpp"

#include "strata/stats.hpp"

#include <algorithm>
#include <format>
#include <fstream>

#include <unistd.h>

namespace strata::soak {
namespace {

constexpr std::size_t kMinSamples = 8;
constexpr double kRssGrowth = 1.10;
constexpr std::uint64_t kRssSlackBytes = 16ULL << 20;
constexpr std::uint64_t kFdSlack = 16;
constexpr std::uint64_t kMemtableFactor = 2;
constexpr double kMiB = 1024.0 * 1024.0;

std::uint64_t ResidentBytes() {
  std::ifstream statm("/proc/self/statm");
  std::uint64_t total_pages = 0;
  std::uint64_t resident_pages = 0;
  statm >> total_pages >> resident_pages;
  return resident_pages * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
}

std::uint64_t OpenDescriptors() {
  std::error_code error;
  std::uint64_t count = 0;
  for (std::filesystem::directory_iterator it("/proc/self/fd", error), end; !error && it != end;
       it.increment(error)) {
    ++count;
  }
  return count;
}

// The largest value of `field` among samples in [from, to) of the run.
template <typename Field>
std::uint64_t MaxIn(const std::vector<ResourceSample>& samples, double from, double to,
                    Field field) {
  std::uint64_t largest = 0;
  for (const ResourceSample& sample : samples) {
    if (sample.seconds >= from && sample.seconds < to) {
      largest = std::max(largest, field(sample));
    }
  }
  return largest;
}

}  // namespace

ResourceSample Sample(const DB& db, double seconds) {
  ResourceSample sample{
      .seconds = seconds, .rss_bytes = ResidentBytes(), .open_fds = OpenDescriptors()};
  if (const Result<Stats> stats = db.GetStats(); stats.ok()) {
    sample.memtable_bytes = stats->gauges.at("memtable.bytes");
    sample.immutable_bytes = stats->gauges.at("immutable_memtable.bytes");
    for (const LevelStats& level : stats->levels) {
      sample.table_files += level.files;
    }
  }
  return sample;
}

std::vector<Check> CheckGrowth(const std::vector<ResourceSample>& samples, const Limits& limits) {
  if (samples.size() < kMinSamples) {
    return {{.name = "samples",
             .measured = std::to_string(samples.size()),
             .bound = ">= " + std::to_string(kMinSamples),
             .passed = false}};
  }
  const double end = samples.back().seconds + 1;
  const double q1 = end / 4;
  const double q2 = end / 2;
  const double q3 = end * 3 / 4;
  const auto rss = [](const ResourceSample& s) { return s.rss_bytes; };
  const auto fds = [](const ResourceSample& s) { return s.open_fds; };
  const std::uint64_t rss_early = MaxIn(samples, q1, q2, rss);
  const std::uint64_t rss_late = MaxIn(samples, q3, end, rss);
  const auto rss_bound =
      static_cast<std::uint64_t>(static_cast<double>(rss_early) * kRssGrowth) + kRssSlackBytes;
  const std::uint64_t fds_early = MaxIn(samples, q1, q2, fds);
  const std::uint64_t fds_late = MaxIn(samples, q3, end, fds);
  std::uint64_t memtable_max = 0;
  for (const ResourceSample& sample : samples) {
    memtable_max = std::max({memtable_max, sample.memtable_bytes, sample.immutable_bytes});
  }
  const std::uint64_t memtable_bound = kMemtableFactor * limits.write_buffer_size;
  return {
      {.name = "memory (RSS) does not grow after warm-up",
       .measured =
           std::format("{:.1f} MiB late vs {:.1f} MiB early", static_cast<double>(rss_late) / kMiB,
                       static_cast<double>(rss_early) / kMiB),
       .bound = std::format("<= {:.1f} MiB", static_cast<double>(rss_bound) / kMiB),
       .passed = rss_late <= rss_bound},
      {.name = "open descriptors do not grow after warm-up",
       .measured = std::format("{} late vs {} early", fds_late, fds_early),
       .bound = std::format("<= {}", fds_early + kFdSlack),
       .passed = fds_late <= fds_early + kFdSlack},
      {.name = "memtables stay bounded",
       .measured = std::format("{:.1f} MiB at most", static_cast<double>(memtable_max) / kMiB),
       .bound = std::format("<= {:.1f} MiB", static_cast<double>(memtable_bound) / kMiB),
       .passed = memtable_max <= memtable_bound},
  };
}

bool WriteReport(const std::filesystem::path& path, const SoakSummary& summary,
                 const std::vector<Check>& checks, const std::vector<ResourceSample>& samples) {
  std::ofstream out(path);
  out << std::format(
      "{{\n  \"seconds\": {:.0f}, \"reopens\": {},\n  \"totals\": {{\"writes\": {}, "
      "\"deletes\": {}, \"reads\": {}, \"scans\": {}, \"snapshots\": {}, \"verified\": {}}},\n"
      "  \"data_error\": \"{}\",\n  \"checks\": [",
      summary.seconds, summary.reopens, summary.writes, summary.deletes, summary.reads,
      summary.scans, summary.snapshots, summary.verified, summary.error.empty() ? "" : "see log");
  for (std::size_t i = 0; i < checks.size(); ++i) {
    out << std::format(
        "{}\n    {{\"name\": \"{}\", \"measured\": \"{}\", \"bound\": \"{}\", "
        "\"passed\": {}}}",
        i == 0 ? "" : ",", checks[i].name, checks[i].measured, checks[i].bound, checks[i].passed);
  }
  out << "\n  ],\n  \"samples\": [";
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const ResourceSample& s = samples[i];
    out << std::format("{}\n    [{:.1f}, {}, {}, {}, {}, {}]", i == 0 ? "" : ",", s.seconds,
                       s.rss_bytes, s.open_fds, s.memtable_bytes, s.immutable_bytes, s.table_files);
  }
  out << "\n  ],\n  \"sample_fields\": [\"seconds\", \"rss_bytes\", \"open_fds\", "
         "\"memtable_bytes\", \"immutable_bytes\", \"table_files\"]\n}\n";
  return static_cast<bool>(out);
}

}  // namespace strata::soak

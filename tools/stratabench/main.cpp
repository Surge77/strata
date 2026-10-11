// stratabench -- the strata benchmark suite (docs/benchmarks.md).
//
// Exit codes: 0 every gated figure stable, 1 engine error, 2 usage error,
// 3 a gated figure was unstable (CV above 5%) and --allow_unstable was not set.

#include <charconv>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "stratabench/bench_db.hpp"
#include "stratabench/report.hpp"

namespace {

using namespace strata::bench;

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr int kExitUnstable = 3;
constexpr std::string_view kLatency = "latency";

struct Args {
  BenchConfig config;
  LatencyConfig latency;
  std::vector<std::string> workloads;
  std::string json_path;
  std::string commit;
  std::string perf_ctl;  // perf record --control fifo:<perf_ctl>,<perf_ack>
  std::string perf_ack;
  bool allow_any_fs = false;
  bool allow_unstable = false;
};

void PrintUsage(std::FILE* out) {
  std::fprintf(out,
               "Usage: stratabench --db=<scratch-dir> [options]\n\n"
               "  --workloads=a,b,...  default: every closed-loop workload; 'latency' runs\n"
               "                       the open-loop harness\n"
               "  --num=N --value_size=N --threads=N --min_round_s=F --seed=N\n"
               "  --warmup=N --rounds=N --cold\n"
               "  --read_fraction=F --workers=N --duration_s=F --rates=auto|r1,r2,...\n"
               "  --json=<path> --commit=<sha> --allow_any_fs --allow_unstable\n"
               "  --perf_ctl=<fifo> --perf_ack=<fifo>  (set by scripts/bench.sh --profile)\n\n"
               "Workloads:");
  for (const auto name : AllWorkloads()) {
    std::fprintf(out, " %.*s", static_cast<int>(name.size()), name.data());
  }
  std::fprintf(out, " latency\n");
}

// Under `perf record -D -1 --control fifo:ctl,ack`, sampling starts disabled
// and is switched on only around each measured workload, so a profile of a
// read workload does not mostly show the load that built its database.
void PerfControl(const Args& args, const char* command) {
  if (args.perf_ctl.empty() || args.perf_ack.empty()) {
    return;
  }
  std::ofstream(args.perf_ctl) << command << '\n' << std::flush;
  std::string ack;
  std::getline(std::ifstream(args.perf_ack), ack);
}

std::vector<std::string> SplitList(std::string_view list) {
  std::vector<std::string> items;
  while (!list.empty()) {
    const auto comma = list.find(',');
    items.emplace_back(list.substr(0, comma));
    list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
  }
  return items;
}

template <typename Number>
bool ParseNumber(std::string_view text, Number* out) {
  const char* end = text.data() + text.size();
  const auto [ptr, error] = std::from_chars(text.data(), end, *out);
  return error == std::errc{} && ptr == end;
}

// A positive number of seconds, e.g. "0.5".
bool ParseSeconds(std::string_view text, std::chrono::nanoseconds* out) {
  constexpr double kNanosPerSecond = 1e9;
  double seconds = 0;
  if (!ParseNumber(text, &seconds) || seconds <= 0) {
    return false;
  }
  *out = std::chrono::nanoseconds(static_cast<std::int64_t>(seconds * kNanosPerSecond));
  return true;
}

// Applies one --name=value flag; false if the flag or its value is invalid.
bool ApplyFlag(std::string_view name, std::string_view value, Args* args) {
  BenchConfig& c = args->config;
  if (name == "db") return !(c.scratch = value).empty();
  if (name == "workloads") return !(args->workloads = SplitList(value)).empty();
  if (name == "num") return ParseNumber(value, &c.num) && c.num > 0;
  if (name == "value_size") return ParseNumber(value, &c.value_size);
  if (name == "threads") return ParseNumber(value, &c.threads) && c.threads > 0;
  if (name == "min_round_s") return ParseSeconds(value, &c.min_round);
  if (name == "seed") return ParseNumber(value, &c.seed);
  if (name == "warmup") return ParseNumber(value, &c.protocol.warmup) && c.protocol.warmup >= 0;
  if (name == "rounds") return ParseNumber(value, &c.protocol.rounds) && c.protocol.rounds > 0;
  if (name == "json") return !(args->json_path = value).empty();
  if (name == "commit") return !(args->commit = value).empty();
  if (name == "perf_ctl") return !(args->perf_ctl = value).empty();
  if (name == "perf_ack") return !(args->perf_ack = value).empty();
  LatencyConfig& l = args->latency;
  if (name == "read_fraction") {
    return ParseNumber(value, &l.read_fraction) && l.read_fraction >= 0 && l.read_fraction <= 1;
  }
  if (name == "workers") return ParseNumber(value, &l.workers) && l.workers > 0;
  if (name == "duration_s") return ParseSeconds(value, &l.duration);
  if (name == "rates") {
    l.rates.clear();
    if (value == "auto") return true;
    for (const std::string& item : SplitList(value)) {
      double rate = 0;
      if (!ParseNumber(std::string_view(item), &rate) || rate <= 0) return false;
      l.rates.push_back(rate);
    }
    return !l.rates.empty();
  }
  return false;
}

std::optional<Args> Parse(int argc, char** argv) {
  Args args;
  for (int index = 1; index < argc; ++index) {
    std::string_view arg = argv[index];
    if (!arg.starts_with("--")) {
      return std::nullopt;
    }
    arg.remove_prefix(2);
    const auto equals = arg.find('=');
    const std::string_view name = arg.substr(0, equals);
    if (equals == std::string_view::npos) {
      bool* toggle = name == "cold"             ? &args.config.cold
                     : name == "allow_any_fs"   ? &args.allow_any_fs
                     : name == "allow_unstable" ? &args.allow_unstable
                                                : nullptr;
      if (toggle == nullptr) return std::nullopt;
      *toggle = true;
    } else if (!ApplyFlag(name, arg.substr(equals + 1), &args)) {
      std::fprintf(stderr, "stratabench: bad flag --%s\n", std::string(arg).c_str());
      return std::nullopt;
    }
  }
  if (args.config.scratch.empty()) {
    return std::nullopt;
  }
  if (args.workloads.empty()) {
    args.workloads.assign(AllWorkloads().begin(), AllWorkloads().end());
  }
  for (const std::string& name : args.workloads) {
    if (!IsKnownWorkload(name) && name != kLatency) {
      std::fprintf(stderr, "stratabench: unknown workload '%s'\n", name.c_str());
      return std::nullopt;
    }
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  const std::optional<Args> parsed = Parse(argc, argv);
  if (!parsed) {
    PrintUsage(stderr);
    return kExitUsage;
  }
  const Args& args = *parsed;
  std::filesystem::create_directories(args.config.scratch);

  Report report{.env = CaptureEnv(args.config.scratch, args.commit), .config = args.config};
  if (report.env.on_9p() && !args.allow_any_fs) {
    std::fprintf(stderr,
                 "stratabench: %s is on a 9P mount (WSL /mnt/c?), whose fsync and caching make "
                 "the numbers meaningless; use an ext4 path or pass --allow_any_fs\n",
                 args.config.scratch.c_str());
    return kExitUsage;
  }

  std::unique_ptr<LoadedDb> loaded;
  for (const std::string& name : args.workloads) {
    if ((NeedsLoadedDb(name) || name == kLatency) && loaded == nullptr) {
      std::fprintf(stderr, "loading %llu keys\n", static_cast<unsigned long long>(args.config.num));
      loaded = std::make_unique<LoadedDb>(args.config.scratch / "loaded", args.config.num,
                                          args.config.value_size, args.config.seed);
      report.db_shape = loaded->Shape();
    }
    PerfControl(args, "enable");
    if (name == kLatency) {
      report.latency = RunLatency(args.config, args.latency, loaded.get());
      PerfControl(args, "disable");
      continue;
    }
    int round = 0;
    const int total = args.config.protocol.warmup + args.config.protocol.rounds;
    const std::vector<double> values = RunRounds(args.config.protocol, [&] {
      std::fprintf(stderr, "%s round %d/%d\n", name.c_str(), round + 1, total);
      return RunWorkloadRound(name, args.config, loaded.get(), round++);
    });
    PerfControl(args, "disable");
    report.workloads.push_back({.name = name,
                                .unit = std::string(UnitOf(name)),
                                .values = values,
                                .summary = Summarize(values)});
  }
  loaded.reset();
  std::filesystem::remove_all(args.config.scratch / "loaded");
  std::filesystem::remove_all(args.config.scratch / "fill");

  PrintReport(report, stdout);
  if (!args.json_path.empty() && !WriteJson(report, args.json_path)) {
    std::fprintf(stderr, "stratabench: cannot write %s\n", args.json_path.c_str());
    return kExitError;
  }
  if (!AllGatedStable(report)) {
    std::fprintf(stderr, "stratabench: a gated figure is UNSTABLE (CV above %.0f%%)\n",
                 100 * kMaxStableCv);
    return args.allow_unstable ? kExitOk : kExitUnstable;
  }
  return kExitOk;
}

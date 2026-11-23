#include "args.hpp"

#include <charconv>
#include <iostream>
#include <string_view>

namespace strata::soak {
namespace {

constexpr std::uint64_t kMaxSeconds = 30ULL * 24 * 3600;
// Keys are "k" and 12 digits (KeyFor), so indexes must stay below 10^12.
constexpr std::uint64_t kMaxKeys = 1'000'000'000'000ULL;
// A value is built in each writer thread; an absurd size would throw there,
// where nothing can catch it, so it is refused while parsing flags.
constexpr std::size_t kMaxValueSize = 1 << 20;

template <typename Number>
bool ParseNumber(std::string_view text, Number* out) {
  const char* end = text.data() + text.size();
  const auto [ptr, error] = std::from_chars(text.data(), end, *out);
  return error == std::errc{} && ptr == end;
}

bool ParseSeconds(std::string_view text, std::uint64_t* out) {
  return ParseNumber(text, out) && *out <= kMaxSeconds;
}

bool ApplyFlag(std::string_view name, std::string_view value, Args* args) {
  auto& w = args->workload;
  if (name == "db") return !(args->db = value).empty();
  if (name == "duration_s") return ParseSeconds(value, &args->duration_s) && args->duration_s > 0;
  if (name == "progress_s") return ParseSeconds(value, &args->progress_s) && args->progress_s > 0;
  if (name == "reopen_every_s") return ParseSeconds(value, &args->reopen_every_s);
  if (name == "sample_s") return ParseSeconds(value, &args->sample_s) && args->sample_s > 0;
  if (name == "check_growth") {
    args->check_growth = value == "1";
    return value == "0" || value == "1";
  }
  if (name == "report") return !(args->report = std::filesystem::path(value))->empty();
  if (name == "serve_port") {
    std::uint16_t port = 0;
    if (!ParseNumber(value, &port)) {
      return false;
    }
    args->serve_port = port;
    return true;
  }
  if (name == "writers") return ParseNumber(value, &w.writers) && w.writers >= 0;
  if (name == "readers") return ParseNumber(value, &w.readers) && w.readers >= 0;
  if (name == "keys") return ParseNumber(value, &w.keys) && w.keys > 0 && w.keys <= kMaxKeys;
  if (name == "value_size") {
    return ParseNumber(value, &w.value_size) && w.value_size <= kMaxValueSize;
  }
  if (name == "sync_every") return ParseNumber(value, &w.sync_every);
  if (name == "delete_every") return ParseNumber(value, &w.delete_every);
  if (name == "verify_every") return ParseNumber(value, &w.verify_every);
  if (name == "scan_every") return ParseNumber(value, &w.scan_every);
  if (name == "seed") return ParseNumber(value, &w.seed);
  return false;
}

}  // namespace

std::optional<Args> ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const std::size_t equals = arg.find('=');
    if (!arg.starts_with("--") || equals == std::string_view::npos ||
        !ApplyFlag(arg.substr(2, equals - 2), arg.substr(equals + 1), &args)) {
      std::cerr << "stratasoak: bad flag '" << arg << "'\n";
      return std::nullopt;
    }
  }
  if (args.db.empty() || args.workload.writers + args.workload.readers == 0) {
    std::cerr << "usage: stratasoak --db=PATH [--duration_s=60] [--serve_port=N]\n"
                 "         [--reopen_every_s=0] [--sample_s=10] [--check_growth=0|1]\n"
                 "         [--report=PATH.json] [--writers=2] [--readers=2] [--keys=200000]\n"
                 "         [--value_size=100] [--sync_every=100] [--delete_every=10]\n"
                 "         [--verify_every=64] [--scan_every=50] [--progress_s=10] [--seed=42]\n";
    return std::nullopt;
  }
  return args;
}

}  // namespace strata::soak

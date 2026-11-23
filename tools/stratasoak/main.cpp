// stratasoak: a mixed workload against one database for a set time.
//
// Writers put and delete keys from a bounded key space (synced now and then);
// readers get keys, scan ranges, and read through snapshots. With
// --serve_port it also serves /stats and the dashboard, which is the quickest
// way to watch the engine work:
//
//   stratasoak --db=/tmp/soak --duration_s=300 --serve_port=8080
//
// Exits 0 if every operation returned ok (or NotFound for a read), 1 on any
// other error, 2 on bad flags.

#include "strata/db.hpp"
#include "strata/options.hpp"
#include "strata/stats.hpp"
#include "strata/stats_server.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "dashboard_files.hpp"
#include "workload.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr std::uint64_t kMaxSeconds = 30ULL * 24 * 3600;
// Keys are "k" and 12 digits (soak::KeyFor), so indexes must stay below 10^12.
constexpr std::uint64_t kMaxKeys = 1'000'000'000'000ULL;
// A value is built in each writer thread; an absurd size would throw there,
// where nothing can catch it, so it is refused while parsing flags.
constexpr std::size_t kMaxValueSize = 1 << 20;

struct Args {
  std::string db;
  std::uint64_t duration_s = 60;
  std::uint64_t progress_s = 10;
  std::optional<std::uint16_t> serve_port;
  strata::soak::WorkloadConfig workload;
};

template <typename Number>
bool ParseNumber(std::string_view text, Number* out) {
  const char* end = text.data() + text.size();
  const auto [ptr, error] = std::from_chars(text.data(), end, *out);
  return error == std::errc{} && ptr == end;
}

bool ApplyFlag(std::string_view name, std::string_view value, Args* args) {
  auto& w = args->workload;
  if (name == "db") return !(args->db = value).empty();
  if (name == "duration_s") {
    return ParseNumber(value, &args->duration_s) && args->duration_s > 0 &&
           args->duration_s <= kMaxSeconds;
  }
  if (name == "progress_s") return ParseNumber(value, &args->progress_s) && args->progress_s > 0;
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
  if (name == "value_size")
    return ParseNumber(value, &w.value_size) && w.value_size <= kMaxValueSize;
  if (name == "sync_every") return ParseNumber(value, &w.sync_every);
  if (name == "delete_every") return ParseNumber(value, &w.delete_every);
  if (name == "scan_every") return ParseNumber(value, &w.scan_every);
  if (name == "seed") return ParseNumber(value, &w.seed);
  return false;
}

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
                 "         [--writers=2] [--readers=2] [--keys=200000] [--value_size=100]\n"
                 "         [--sync_every=100] [--delete_every=10] [--scan_every=50]\n"
                 "         [--progress_s=10] [--seed=42]\n";
    return std::nullopt;
  }
  return args;
}

void PrintProgress(std::uint64_t elapsed_s, const strata::soak::Totals& totals,
                   const strata::DB& db) {
  std::cout << "t=" << elapsed_s << "s writes=" << totals.writes << " deletes=" << totals.deletes
            << " reads=" << totals.reads << " scans=" << totals.scans
            << " snapshots=" << totals.snapshots;
  if (const auto stats = db.GetStats(); stats.ok()) {
    std::cout << " memtable=" << stats->gauges.at("memtable.bytes") << "B levels=";
    for (const strata::LevelStats& level : stats->levels) {
      std::cout << level.files
                << (level.level + 1 < static_cast<int>(stats->levels.size()) ? "," : "");
    }
  }
  std::cout << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  const std::optional<Args> args = ParseArgs(argc, argv);
  if (!args.has_value()) {
    return kExitUsage;
  }
  // Blocked before any thread starts, so only the sigtimedwait below sees
  // Ctrl-C and SIGTERM, and the run ends as if its time were up.
  sigset_t stop_signals;
  sigemptyset(&stop_signals);
  sigaddset(&stop_signals, SIGINT);
  sigaddset(&stop_signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

  // Open creates only the last directory of the path, and a soak's database
  // usually sits under a scratch directory that may not exist yet.
  std::error_code ignored;
  std::filesystem::create_directories(std::filesystem::path(args->db).parent_path(), ignored);
  strata::DB db;
  if (const strata::Status opened = strata::DB::Open({.path = args->db}, &db); !opened.ok()) {
    std::cerr << "stratasoak: " << opened.ToString() << '\n';
    return kExitFailed;
  }
  std::unique_ptr<strata::StatsServer> server;
  if (args->serve_port.has_value()) {
    auto started = strata::StatsServer::Start(
        &db, {.port = *args->serve_port, .files = strata::dashboard::Files()});
    if (!started.ok()) {
      std::cerr << "stratasoak: " << started.status().ToString() << '\n';
      return kExitFailed;
    }
    server = std::move(*started);
    std::cout << "dashboard http://127.0.0.1:" << server->port() << "/" << std::endl;
  }

  strata::soak::Workload workload(&db, args->workload);
  workload.Start();
  const auto start = std::chrono::steady_clock::now();
  const auto end = start + std::chrono::seconds(args->duration_s);
  auto next_progress = start + std::chrono::seconds(args->progress_s);
  while (!workload.totals().failed.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= end) {
      break;
    }
    const auto wait = std::min(end, next_progress) - now;
    const auto wait_s = std::chrono::duration_cast<std::chrono::seconds>(wait);
    const timespec timeout{
        .tv_sec = wait_s.count(),
        .tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(wait - wait_s).count()};
    if (sigtimedwait(&stop_signals, nullptr, &timeout) > 0) {
      std::cout << "interrupted" << std::endl;
      break;
    }
    if (std::chrono::steady_clock::now() >= next_progress) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - start);
      PrintProgress(static_cast<std::uint64_t>(elapsed.count()), workload.totals(), db);
      next_progress += std::chrono::seconds(args->progress_s);
    }
  }
  workload.Stop();
  const auto ran =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);
  PrintProgress(static_cast<std::uint64_t>(ran.count()), workload.totals(), db);
  // The default dispositions come back before the server stops and the
  // database closes, so a second Ctrl-C during that shutdown ends the process
  // (as in stratactl serve) rather than being ignored.
  pthread_sigmask(SIG_UNBLOCK, &stop_signals, nullptr);
  if (server != nullptr) {
    server->Stop();
  }
  const strata::Status closed = db.Close();
  if (workload.totals().failed.load()) {
    std::cerr << "stratasoak: FAILED: " << workload.first_error() << '\n';
    return kExitFailed;
  }
  if (!closed.ok()) {
    std::cerr << "stratasoak: close: " << closed.ToString() << '\n';
    return kExitFailed;
  }
  std::cout << "ok" << std::endl;
  return kExitOk;
}

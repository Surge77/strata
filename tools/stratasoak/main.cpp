// stratasoak: a mixed workload against one database for a set time, and the
// release soak test.
//
// Writers put and delete keys from a bounded key space (synced now and then),
// each in its own stripe of keys, and read their own keys back against a
// model of what they wrote; readers get keys, scan ranges, and read through
// snapshots. Optionally it reopens the database on a schedule (checking every
// key the writers ever touched), samples memory, descriptors, and memtables,
// and judges whether any of them keeps growing. With --serve_port it also
// serves /stats and the dashboard:
//
//   stratasoak --db=/tmp/soak --duration_s=300 --serve_port=8080
//   stratasoak --db=/tmp/soak --duration_s=3600 --reopen_every_s=600
//              --check_growth=1 --report=soak.json   (the release soak)
//
// Exits 0 if every operation and check passed, 1 otherwise, 2 on bad flags.

#include "strata/db.hpp"
#include "strata/options.hpp"
#include "strata/stats.hpp"
#include "strata/stats_server.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <system_error>
#include <vector>

#include "args.hpp"
#include "dashboard_files.hpp"
#include "monitor.hpp"
#include "workload.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using strata::soak::Args;

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;

void PrintProgress(double elapsed_s, const strata::soak::Totals& totals, const strata::DB& db) {
  std::cout << "t=" << static_cast<std::uint64_t>(elapsed_s) << "s writes=" << totals.writes
            << " deletes=" << totals.deletes << " reads=" << totals.reads
            << " scans=" << totals.scans << " snapshots=" << totals.snapshots
            << " verified=" << totals.verified;
  if (const auto stats = db.GetStats(); stats.ok()) {
    std::cout << " memtable=" << stats->gauges.at("memtable.bytes") << "B levels=";
    for (const strata::LevelStats& level : stats->levels) {
      std::cout << level.files
                << (level.level + 1 < static_cast<int>(stats->levels.size()) ? "," : "");
    }
  }
  std::cout << std::endl;
}

// Opens the database and, if asked, a stats server in front of it.
class Engine {
 public:
  explicit Engine(const Args& args) : args_(args) {}

  strata::Status Open() {
    STRATA_RETURN_IF_ERROR(strata::DB::Open({.path = args_.db}, &db_));
    if (!args_.serve_port.has_value()) {
      return strata::Status::Ok();
    }
    auto started = strata::StatsServer::Start(
        &db_, {.port = port_.value_or(*args_.serve_port), .files = strata::dashboard::Files()});
    if (!started.ok()) {
      return started.status();
    }
    server_ = std::move(*started);
    if (!port_.has_value()) {
      port_ = server_->port();  // keep the same port across reopens
      std::cout << "dashboard http://127.0.0.1:" << *port_ << "/" << std::endl;
    }
    return strata::Status::Ok();
  }

  // The server goes first: it calls the database from its own thread.
  strata::Status Close() {
    server_.reset();
    return db_.Close();
  }

  strata::DB& db() { return db_; }

 private:
  const Args& args_;
  strata::DB db_;
  std::unique_ptr<strata::StatsServer> server_;
  std::optional<std::uint16_t> port_;
};

bool AllPassed(const std::vector<strata::soak::Check>& checks) {
  return std::ranges::all_of(checks, [](const auto& check) { return check.passed; });
}

}  // namespace

int main(int argc, char** argv) {
  const std::optional<Args> args = strata::soak::ParseArgs(argc, argv);
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

  // Open creates only the last directory of the path.
  std::error_code ignored;
  std::filesystem::create_directories(std::filesystem::path(args->db).parent_path(), ignored);
  Engine engine(*args);
  if (const strata::Status opened = engine.Open(); !opened.ok()) {
    std::cerr << "stratasoak: " << opened.ToString() << '\n';
    return kExitFailed;
  }

  strata::soak::Workload workload(&engine.db(), args->workload);
  std::vector<strata::soak::ResourceSample> samples;
  std::string data_error;
  int reopens = 0;
  workload.Start();
  const auto start = Clock::now();
  const auto seconds_since_start = [&] {
    return std::chrono::duration<double>(Clock::now() - start).count();
  };
  const auto every = [](std::uint64_t s) { return std::chrono::seconds(s); };
  const auto end = start + every(args->duration_s);
  auto next_progress = start + every(args->progress_s);
  auto next_sample = start;
  auto next_reopen =
      args->reopen_every_s == 0 ? Clock::time_point::max() : start + every(args->reopen_every_s);
  while (!workload.totals().failed.load()) {
    const auto now = Clock::now();
    if (now >= end) {
      break;
    }
    if (now >= next_sample) {
      samples.push_back(strata::soak::Sample(engine.db(), seconds_since_start()));
      next_sample += every(args->sample_s);
    }
    if (now >= next_progress) {
      PrintProgress(seconds_since_start(), workload.totals(), engine.db());
      next_progress += every(args->progress_s);
    }
    if (now >= next_reopen) {
      workload.Stop();
      strata::Status status = engine.Close();
      status = status.ok() ? engine.Open() : status;
      status = status.ok() ? workload.VerifyAll() : status;
      if (!status.ok()) {
        data_error = "reopen " + std::to_string(reopens + 1) + ": " + status.ToString();
        break;
      }
      ++reopens;
      std::cout << "reopened (" << reopens << "), every key checked" << std::endl;
      workload.Start();
      next_reopen += every(args->reopen_every_s);
    }
    const auto wake = std::min({end, next_progress, next_sample, next_reopen});
    const auto wait = std::max(wake - Clock::now(), Clock::duration::zero());
    const auto wait_s = std::chrono::duration_cast<std::chrono::seconds>(wait);
    const timespec timeout{
        .tv_sec = wait_s.count(),
        .tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(wait - wait_s).count()};
    if (sigtimedwait(&stop_signals, nullptr, &timeout) > 0) {
      std::cout << "interrupted" << std::endl;
      break;
    }
  }
  workload.Stop();
  const double ran = seconds_since_start();
  PrintProgress(ran, workload.totals(), engine.db());
  if (data_error.empty() && workload.totals().failed.load()) {
    data_error = workload.first_error();
  }
  if (data_error.empty()) {
    if (const strata::Status verified = workload.VerifyAll(); !verified.ok()) {
      data_error = "final check: " + verified.ToString();
    }
  }
  // The default dispositions come back before the server stops and the
  // database closes, so a second Ctrl-C during that shutdown ends the process
  // (as in stratactl serve) rather than being ignored.
  pthread_sigmask(SIG_UNBLOCK, &stop_signals, nullptr);
  const strata::Status closed = engine.Close();
  if (data_error.empty() && !closed.ok()) {
    data_error = "close: " + closed.ToString();
  }

  const strata::soak::Limits limits{.write_buffer_size = strata::Options{}.write_buffer_size};
  const auto checks = args->check_growth ? strata::soak::CheckGrowth(samples, limits)
                                         : std::vector<strata::soak::Check>{};
  for (const auto& check : checks) {
    std::cout << (check.passed ? "PASS " : "FAIL ") << check.name << ": " << check.measured << " ("
              << check.bound << ")" << std::endl;
  }
  if (args->report.has_value()) {
    const auto& t = workload.totals();
    const strata::soak::SoakSummary summary{.seconds = ran,
                                            .reopens = reopens,
                                            .writes = t.writes,
                                            .deletes = t.deletes,
                                            .reads = t.reads,
                                            .scans = t.scans,
                                            .snapshots = t.snapshots,
                                            .verified = t.verified,
                                            .error = data_error};
    if (!strata::soak::WriteReport(*args->report, summary, checks, samples)) {
      std::cerr << "stratasoak: cannot write " << args->report->string() << '\n';
    }
  }
  if (!data_error.empty()) {
    std::cerr << "stratasoak: FAILED: " << data_error << '\n';
    return kExitFailed;
  }
  if (!AllPassed(checks)) {
    std::cerr << "stratasoak: FAILED: a growth check\n";
    return kExitFailed;
  }
  std::cout << "ok" << std::endl;
  return kExitOk;
}

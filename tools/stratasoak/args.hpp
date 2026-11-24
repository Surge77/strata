#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "workload.hpp"

namespace strata::soak {

struct Args {
  std::string db;
  std::uint64_t duration_s = 60;
  std::uint64_t progress_s = 10;
  // Every this many seconds the workload stops, the database is closed and
  // reopened, every key the writers touched is checked, and the workload
  // resumes. 0 never reopens.
  std::uint64_t reopen_every_s = 0;
  // Resource samples (memory, descriptors, memtables) every this many seconds.
  std::uint64_t sample_s = 10;
  // Enforce the growth checks (monitor.hpp); the release soak sets it.
  bool check_growth = false;
  std::optional<std::filesystem::path> report;
  std::optional<std::uint16_t> serve_port;
  WorkloadConfig workload;
};

// Nothing (after printing why) for bad or missing flags.
[[nodiscard]] std::optional<Args> ParseArgs(int argc, char** argv);

}  // namespace strata::soak

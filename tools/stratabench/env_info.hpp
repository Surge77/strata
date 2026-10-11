#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace strata::bench {

// The environment a run measured, recorded at the top of every report so a
// number is never separated from the machine and build that produced it.
struct EnvInfo {
  std::string timestamp_utc;  // ISO 8601
  std::string commit;         // passed in by scripts/bench.sh; "unknown" otherwise
  std::string cpu_model;
  unsigned logical_cpus = 0;
  std::uint64_t memory_mib = 0;
  std::string kernel;
  bool wsl = false;
  std::string compiler;
  std::string build_type;
  std::string sanitizer;
  std::string filesystem;  // of the scratch directory: "ext4", "9p", ...

  // An optimised, uninstrumented build: the only kind whose numbers mean
  // anything about the engine.
  [[nodiscard]] bool publishable_build() const;
  // The scratch directory is on a 9P mount (WSL's /mnt/c), whose fsync and
  // caching make durability and timing meaningless.
  [[nodiscard]] bool on_9p() const { return filesystem == "9p"; }
};

[[nodiscard]] EnvInfo CaptureEnv(const std::filesystem::path& scratch, std::string commit);

// The filesystem type of `path` from statfs: a name for the common ones, the
// magic number in hex otherwise.
[[nodiscard]] std::string FilesystemType(const std::filesystem::path& path);

}  // namespace strata::bench

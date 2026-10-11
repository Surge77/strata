#include "stratabench/env_info.hpp"

#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <thread>
#include <utility>

#include <sys/statfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef STRATA_BENCH_BUILD_TYPE
#define STRATA_BENCH_BUILD_TYPE "unknown"
#endif
#ifndef STRATA_BENCH_SANITIZER
#define STRATA_BENCH_SANITIZER "unknown"
#endif

namespace strata::bench {
namespace {

struct FilesystemMagic {
  std::uint64_t magic;
  const char* name;
};

// From linux/magic.h.
constexpr std::array<FilesystemMagic, 8> kFilesystems = {{
    {0xEF53, "ext4"},
    {0x01021997, "9p"},
    {0x01021994, "tmpfs"},
    {0x9123683E, "btrfs"},
    {0x58465342, "xfs"},
    {0x794C7630, "overlay"},
    {0x6969, "nfs"},
    {0x65735546, "fuse"},
}};

constexpr std::uint64_t kKibPerMib = 1024;

// The value after "<key> :" on the first line of `path` that starts with `key`.
std::string FirstField(const char* path, std::string_view key) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.starts_with(key)) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) {
        return {};
      }
      const auto start = line.find_first_not_of(" \t", colon + 1);
      return start == std::string::npos ? std::string{} : line.substr(start);
    }
  }
  return {};
}

std::string UtcTimestamp() {
  const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

}  // namespace

bool EnvInfo::publishable_build() const {
  return (build_type == "Release" || build_type == "RelWithDebInfo") && sanitizer == "none";
}

std::string FilesystemType(const std::filesystem::path& path) {
  struct statfs info {};
  if (::statfs(path.c_str(), &info) != 0) {
    return "unknown";
  }
  const auto magic = static_cast<std::uint64_t>(info.f_type);
  for (const auto& known : kFilesystems) {
    if (known.magic == magic) {
      return known.name;
    }
  }
  return std::format("0x{:x}", magic);
}

EnvInfo CaptureEnv(const std::filesystem::path& scratch, std::string commit) {
  EnvInfo env;
  env.timestamp_utc = UtcTimestamp();
  env.commit = commit.empty() ? "unknown" : std::move(commit);
  env.cpu_model = FirstField("/proc/cpuinfo", "model name");
  env.logical_cpus = std::thread::hardware_concurrency();
  const std::string mem_kib = FirstField("/proc/meminfo", "MemTotal");
  env.memory_mib = mem_kib.empty() ? 0 : std::stoull(mem_kib) / kKibPerMib;
  utsname names{};
  if (::uname(&names) == 0) {
    env.kernel = std::string(names.sysname) + " " + names.release;
    env.wsl = env.kernel.find("microsoft") != std::string::npos ||
              env.kernel.find("WSL") != std::string::npos;
  }
#if defined(__clang__)
  env.compiler =
      std::format("clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
  env.compiler = std::format("gcc {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
  env.compiler = "unknown";
#endif
  env.build_type = STRATA_BENCH_BUILD_TYPE;
  env.sanitizer = STRATA_BENCH_SANITIZER;
  env.filesystem = FilesystemType(scratch);
  return env;
}

}  // namespace strata::bench

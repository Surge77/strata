#pragma once

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <unistd.h>

namespace strata::test {

// A fresh, empty directory for one test, removed when the test ends.
//
// Created under $STRATA_TEST_DIR (set by scripts/test.sh to a path on ext4)
// because durability tests depend on real fsync semantics, which a /mnt/c 9P
// mount does not provide. Falls back to /tmp when run outside the script.
// The name includes the process id and a counter so tests running in parallel
// under ctest never share a directory.
class TestDir {
 public:
  explicit TestDir(std::string_view name) {
    static std::atomic<int> counter{0};
    const char* const base = std::getenv("STRATA_TEST_DIR");
    const std::filesystem::path root = base != nullptr ? base : "/tmp/strata-tests";
    path_ = (root / (std::string(name) + "-" + std::to_string(::getpid()) + "-" +
                     std::to_string(counter.fetch_add(1))))
                .string();
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  ~TestDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TestDir(const TestDir&) = delete;
  TestDir& operator=(const TestDir&) = delete;
  TestDir(TestDir&&) = delete;
  TestDir& operator=(TestDir&&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  // Path of a file inside the directory.
  [[nodiscard]] std::string File(std::string_view name) const {
    return path_ + "/" + std::string(name);
  }

 private:
  std::string path_;
};

}  // namespace strata::test

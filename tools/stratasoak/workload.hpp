#pragma once

#include "strata/db.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace strata::soak {

struct WorkloadConfig {
  int writers = 2;
  int readers = 2;
  // Keys are drawn from [0, keys), so writes overwrite and delete earlier
  // ones and compaction has garbage to drop.
  std::uint64_t keys = 200'000;
  std::size_t value_size = 100;
  // One write in this many is synced; 0 syncs none.
  std::uint64_t sync_every = 100;
  // One write in this many is a delete; 0 deletes none.
  std::uint64_t delete_every = 10;
  // One reader operation in this many is a 100-key scan; another in this many
  // takes a snapshot, reads through it, and releases it.
  std::uint64_t scan_every = 50;
  std::uint64_t seed = 42;
};

// Operation totals, safe to read while the workload runs.
struct Totals {
  std::atomic<std::uint64_t> writes{0};
  std::atomic<std::uint64_t> deletes{0};
  std::atomic<std::uint64_t> reads{0};
  std::atomic<std::uint64_t> scans{0};
  std::atomic<std::uint64_t> snapshots{0};
  // Any status that is neither ok nor NotFound. Set once; the workload stops.
  std::atomic<bool> failed{false};
};

// Threads hammering one open database until Stop. The database must outlive
// the workload.
class Workload {
 public:
  Workload(DB* db, const WorkloadConfig& config);
  ~Workload();
  Workload(const Workload&) = delete;
  Workload& operator=(const Workload&) = delete;
  Workload(Workload&&) = delete;
  Workload& operator=(Workload&&) = delete;

  void Start();
  // Signals every thread and joins them. Idempotent.
  void Stop();

  [[nodiscard]] const Totals& totals() const noexcept { return totals_; }
  // The first unexpected error, once totals().failed is set and Stop returned.
  [[nodiscard]] const std::string& first_error() const noexcept { return first_error_; }

 private:
  void Write(int index);
  void Read(int index);
  void Fail(const Status& status);

  DB* const db_;
  const WorkloadConfig config_;
  Totals totals_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> error_claimed_{false};
  std::string first_error_;
  std::vector<std::thread> threads_;
};

// "k000000012345": fixed width, so keys sort in numeric order.
[[nodiscard]] std::string KeyFor(std::uint64_t index);

}  // namespace strata::soak

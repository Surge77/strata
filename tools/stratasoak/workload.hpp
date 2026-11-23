#pragma once

#include "strata/db.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace strata::soak {

struct WorkloadConfig {
  int writers = 2;
  int readers = 2;
  // Keys are drawn from [0, keys), so writes overwrite and delete earlier
  // ones and compaction has garbage to drop. Writer w owns the keys whose
  // index is w modulo `writers`, so it alone knows what each should hold.
  std::uint64_t keys = 200'000;
  std::size_t value_size = 100;
  // One write in this many is synced; 0 syncs none.
  std::uint64_t sync_every = 100;
  // One write in this many is a delete; 0 deletes none.
  std::uint64_t delete_every = 10;
  // After this many writes, a writer reads back one of its keys and checks it
  // against what it last wrote there; 0 never checks.
  std::uint64_t verify_every = 64;
  // One reader operation in this many is a 100-key scan; another in this many
  // takes a snapshot, reads through it, and releases it.
  std::uint64_t scan_every = 50;
  std::uint64_t seed = 42;
};

// Operation totals, safe to read while the workload runs. They accumulate
// over every Start/Stop cycle.
struct Totals {
  std::atomic<std::uint64_t> writes{0};
  std::atomic<std::uint64_t> deletes{0};
  std::atomic<std::uint64_t> reads{0};
  std::atomic<std::uint64_t> scans{0};
  std::atomic<std::uint64_t> snapshots{0};
  std::atomic<std::uint64_t> verified{0};
  // Any status that is neither ok nor NotFound, or a read that disagrees
  // with the model. Set once; the workload stops.
  std::atomic<bool> failed{false};
};

// Threads hammering one open database until Stop. Start may be called again
// after Stop (for instance after the database was closed and reopened); the
// writers' models carry over, so what was written before is still checked.
// The database must outlive the workload.
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

  // With the workload stopped: checks every key every writer has touched.
  // Ok, or the first mismatch as Corruption (or the read's own error).
  [[nodiscard]] Status VerifyAll();

  [[nodiscard]] const Totals& totals() const noexcept { return totals_; }
  // The first unexpected error, once totals().failed is set and Stop returned.
  [[nodiscard]] const std::string& first_error() const noexcept { return first_error_; }

 private:
  // What one writer has written: key index -> version, 0 for deleted.
  struct Model {
    std::unordered_map<std::uint64_t, std::uint64_t> versions;
    std::uint64_t next_version = 1;
    std::uint64_t ops = 0;
  };

  void Write(int index);
  void Read(int index);
  void Fail(const Status& status);
  [[nodiscard]] Status Check(std::uint64_t key_index, std::uint64_t version) const;

  DB* const db_;
  const WorkloadConfig config_;
  Totals totals_;
  std::vector<Model> models_;  // one per writer, touched only by its writer
  std::atomic<bool> stop_{false};
  std::atomic<bool> error_claimed_{false};
  std::string first_error_;
  std::vector<std::thread> threads_;
  std::uint64_t cycle_ = 0;  // Start count, so each cycle's random streams differ
};

// "k000000012345": fixed width, so keys sort in numeric order.
[[nodiscard]] std::string KeyFor(std::uint64_t index);

// The value version `version` of key `index` holds: half random letters
// (from a stream seeded by both), half a repeated byte.
[[nodiscard]] std::string ValueFor(std::uint64_t index, std::uint64_t version, std::size_t size);

}  // namespace strata::soak

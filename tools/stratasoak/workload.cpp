#include "workload.hpp"

#include "strata/iterator.hpp"
#include "strata/options.hpp"

#include <cstdio>
#include <random>

namespace strata::soak {
namespace {

constexpr int kKeyDigits = 12;
constexpr int kScanLength = 100;

// Half random letters, half a repeated pattern: neither all noise nor all
// redundancy, so compression (phase 14) has something realistic to work on.
std::string ValueFor(std::mt19937_64& random, std::size_t size) {
  std::string value(size, 'v');
  std::uniform_int_distribution<int> letter('a', 'z');
  for (std::size_t i = 0; i < size / 2; ++i) {
    value[i] = static_cast<char>(letter(random));
  }
  return value;
}

}  // namespace

std::string KeyFor(std::uint64_t index) {
  std::string key(kKeyDigits + 2, '\0');
  const int written = std::snprintf(key.data(), key.size(), "k%0*llu", kKeyDigits,
                                    static_cast<unsigned long long>(index));
  key.resize(static_cast<std::size_t>(written));
  return key;
}

Workload::Workload(DB* db, const WorkloadConfig& config) : db_(db), config_(config) {}

Workload::~Workload() {
  Stop();
}

void Workload::Start() {
  for (int i = 0; i < config_.writers; ++i) {
    threads_.emplace_back([this, i] { Write(i); });
  }
  for (int i = 0; i < config_.readers; ++i) {
    threads_.emplace_back([this, i] { Read(i); });
  }
}

void Workload::Stop() {
  stop_ = true;
  for (std::thread& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

void Workload::Fail(const Status& status) {
  if (!error_claimed_.exchange(true)) {
    first_error_ = status.ToString();
    totals_.failed = true;
  }
  stop_ = true;
}

void Workload::Write(int index) {
  std::mt19937_64 random(config_.seed + static_cast<std::uint64_t>(index));
  std::uniform_int_distribution<std::uint64_t> key_index(0, config_.keys - 1);
  for (std::uint64_t op = 1; !stop_.load(std::memory_order_relaxed); ++op) {
    const std::string key = KeyFor(key_index(random));
    const WriteOptions options{.sync = config_.sync_every != 0 && op % config_.sync_every == 0};
    const bool remove = config_.delete_every != 0 && op % config_.delete_every == 0;
    const Status status = remove ? db_->Delete(key, options)
                                 : db_->Put(key, ValueFor(random, config_.value_size), options);
    if (!status.ok()) {
      Fail(status);
      return;
    }
    (remove ? totals_.deletes : totals_.writes).fetch_add(1, std::memory_order_relaxed);
  }
}

void Workload::Read(int index) {
  std::mt19937_64 random(config_.seed + 1000 + static_cast<std::uint64_t>(index));
  std::uniform_int_distribution<std::uint64_t> key_index(0, config_.keys - 1);
  for (std::uint64_t op = 1; !stop_.load(std::memory_order_relaxed); ++op) {
    const std::string key = KeyFor(key_index(random));
    if (config_.scan_every != 0 && op % config_.scan_every == 0) {
      auto iterator = db_->NewIterator();
      iterator->Seek(key);
      for (int i = 0; i < kScanLength && iterator->Valid(); ++i) {
        iterator->Next();
      }
      if (!iterator->status().ok()) {
        Fail(iterator->status());
        return;
      }
      totals_.scans.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (config_.scan_every != 0 && op % config_.scan_every == 1) {
      const Snapshot* snapshot = db_->GetSnapshot();
      const Result<std::string> value = db_->Get(key, {.snapshot = snapshot});
      const Status released = db_->ReleaseSnapshot(snapshot);
      if ((!value.ok() && !value.status().IsNotFound()) || !released.ok()) {
        Fail(value.ok() ? released : value.status());
        return;
      }
      totals_.snapshots.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const Result<std::string> value = db_->Get(key);
    if (!value.ok() && !value.status().IsNotFound()) {
      Fail(value.status());
      return;
    }
    totals_.reads.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace strata::soak

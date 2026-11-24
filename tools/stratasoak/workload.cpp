#include "workload.hpp"

#include "strata/iterator.hpp"
#include "strata/options.hpp"

#include <cstdio>
#include <random>

namespace strata::soak {
namespace {

constexpr int kKeyDigits = 12;
constexpr int kScanLength = 100;
// Separates the seeds of writer, reader, and value streams.
constexpr std::uint64_t kReaderSeedOffset = 1000;
constexpr std::uint64_t kCycleSeedStride = 7919;

}  // namespace

std::string KeyFor(std::uint64_t index) {
  std::string key(kKeyDigits + 2, '\0');
  const int written = std::snprintf(key.data(), key.size(), "k%0*llu", kKeyDigits,
                                    static_cast<unsigned long long>(index));
  key.resize(static_cast<std::size_t>(written));
  return key;
}

std::string ValueFor(std::uint64_t index, std::uint64_t version, std::size_t size) {
  std::mt19937_64 random(index * 0x9E3779B97F4A7C15ULL ^ version);
  std::uniform_int_distribution<int> letter('a', 'z');
  std::string value(size, 'v');
  for (std::size_t i = 0; i < size / 2; ++i) {
    value[i] = static_cast<char>(letter(random));
  }
  return value;
}

Workload::Workload(DB* db, const WorkloadConfig& config)
    : db_(db), config_(config), models_(static_cast<std::size_t>(config.writers)) {}

Workload::~Workload() {
  Stop();
}

void Workload::Start() {
  stop_ = false;
  ++cycle_;
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
  threads_.clear();
}

void Workload::Fail(const Status& status) {
  if (!error_claimed_.exchange(true)) {
    first_error_ = status.ToString();
    totals_.failed = true;
  }
  stop_ = true;
}

Status Workload::Check(std::uint64_t key_index, std::uint64_t version) const {
  const Result<std::string> value = db_->Get(KeyFor(key_index));
  if (version == 0) {
    if (value.ok()) {
      return Status::Corruption("deleted key is back: " + KeyFor(key_index));
    }
    return value.status().IsNotFound() ? Status::Ok() : value.status();
  }
  if (!value.ok()) {
    return value.status().IsNotFound()
               ? Status::Corruption("acknowledged write is missing: " + KeyFor(key_index))
               : value.status();
  }
  if (*value != ValueFor(key_index, version, config_.value_size)) {
    return Status::Corruption("wrong value for " + KeyFor(key_index));
  }
  return Status::Ok();
}

Status Workload::VerifyAll() {
  for (const Model& model : models_) {
    for (const auto& [key_index, version] : model.versions) {
      STRATA_RETURN_IF_ERROR(Check(key_index, version));
      totals_.verified.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return Status::Ok();
}

void Workload::Write(int index) {
  Model& model = models_[static_cast<std::size_t>(index)];
  const auto writers = static_cast<std::uint64_t>(config_.writers);
  const std::uint64_t stripe_keys =
      (config_.keys - static_cast<std::uint64_t>(index) + writers - 1) / writers;
  if (stripe_keys == 0) {
    return;
  }
  std::mt19937_64 random(config_.seed + static_cast<std::uint64_t>(index) +
                         cycle_ * kCycleSeedStride);
  std::uniform_int_distribution<std::uint64_t> slot(0, stripe_keys - 1);
  while (!stop_.load(std::memory_order_relaxed)) {
    const std::uint64_t op = ++model.ops;
    const std::uint64_t key_index = slot(random) * writers + static_cast<std::uint64_t>(index);
    const WriteOptions options{.sync = config_.sync_every != 0 && op % config_.sync_every == 0};
    const bool remove = config_.delete_every != 0 && op % config_.delete_every == 0;
    const std::uint64_t version = remove ? 0 : model.next_version++;
    const Status status = remove
                              ? db_->Delete(KeyFor(key_index), options)
                              : db_->Put(KeyFor(key_index),
                                         ValueFor(key_index, version, config_.value_size), options);
    if (!status.ok()) {
      Fail(status);
      return;
    }
    model.versions[key_index] = version;
    (remove ? totals_.deletes : totals_.writes).fetch_add(1, std::memory_order_relaxed);
    if (config_.verify_every != 0 && op % config_.verify_every == 0) {
      // Read back a random key of this writer's stripe: no other thread writes
      // it, so the model is exactly what the database must return. A key never
      // written must be absent, like a deleted one.
      const std::uint64_t checked = slot(random) * writers + static_cast<std::uint64_t>(index);
      const auto found = model.versions.find(checked);
      const std::uint64_t expected = found == model.versions.end() ? 0 : found->second;
      if (const Status verified = Check(checked, expected); !verified.ok()) {
        Fail(verified);
        return;
      }
      totals_.verified.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Workload::Read(int index) {
  std::mt19937_64 random(config_.seed + kReaderSeedOffset + static_cast<std::uint64_t>(index) +
                         cycle_ * kCycleSeedStride);
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

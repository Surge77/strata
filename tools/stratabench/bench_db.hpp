#pragma once

#include "strata/slice.hpp"
#include "strata/status.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "db/db_impl.hpp"

namespace strata::bench {

// Keys are exactly kKeySize decimal digits, so their byte order is their
// numeric order and every key in a run has the same size.
inline constexpr std::size_t kKeySize = 16;

struct KeyBuffer {
  char bytes[kKeySize + 1];  // + 1 for the "missing key" suffix
};

// Writes `number` as kKeySize zero-padded digits; returns a view of them.
// Hand-rolled because snprintf costs more than some of the operations timed.
Slice FormatKey(std::uint64_t number, KeyBuffer* buffer) noexcept;

// A key that sorts directly after FormatKey(number) and never exists: the
// same digits plus one byte. Lookups for it walk the same tables as a hit.
Slice FormatMissingKey(std::uint64_t number, KeyBuffer* buffer) noexcept;

// Value bytes for writes: windows into a fixed pseudo-random buffer, so values
// are neither all one byte nor regenerated per write. Const and thread-safe.
class ValueSource {
 public:
  ValueSource(std::size_t value_size, std::uint64_t seed);
  // The value for write number `index`.
  [[nodiscard]] Slice At(std::uint64_t index) const noexcept;

 private:
  std::string data_;
  std::size_t value_size_;
};

// Prints `what: status` to stderr and exits with status 1. A benchmark that
// hit an engine error has no result worth returning.
[[noreturn]] void Fail(const char* what, const Status& status);

// Removes `path` and opens an empty database there.
std::unique_ptr<DBImpl> OpenFresh(const std::filesystem::path& path);
std::unique_ptr<DBImpl> OpenExisting(const std::filesystem::path& path);
void CloseOrFail(DBImpl* db);

// Asks the kernel to drop every file under `path` from the page cache
// (posix_fadvise DONTNEED). Needs no privileges; works for clean pages only,
// which after a close is all of them.
void EvictFromPageCache(const std::filesystem::path& path);

// Writes back every dirty page of the filesystem holding `path` (syncfs), so
// setup work leaves no I/O to be paid for inside a timed round.
void WriteBackFilesystem(const std::filesystem::path& path);

// The shared database the loaded workloads use: `keys` keys, each written once
// in random order, with compactions waited out, so reads go through a settled
// multi-level tree. The loaded state is kept as a pristine copy under `path`;
// workloads run against a work copy of it.
class LoadedDb {
 public:
  LoadedDb(const std::filesystem::path& path, std::uint64_t keys, std::size_t value_size,
           std::uint64_t seed);

  [[nodiscard]] DBImpl* db() const noexcept { return db_.get(); }
  [[nodiscard]] std::uint64_t keys() const noexcept { return keys_; }

  // Before each measured round. A round that `writes` -- or any round after
  // one that did -- starts from a fresh copy of the pristine database, so
  // every round of a writing workload is the same experiment rather than a
  // later one on a bigger tree. With `cold`, the database is then closed, its
  // files are evicted from the page cache, and it is reopened, which also
  // empties the block cache.
  void PrepareRound(bool cold, bool writes);

  // "5 49 2" -- tables per level, for the report.
  [[nodiscard]] std::string Shape() const;

 private:
  void ResetWorkCopy();

  std::filesystem::path pristine_;
  std::filesystem::path work_;
  std::uint64_t keys_;
  std::unique_ptr<DBImpl> db_;
  bool dirty_ = false;  // the work copy was written to since its last reset
};

}  // namespace strata::bench

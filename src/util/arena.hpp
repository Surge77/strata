#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace strata {

// A bump allocator whose entire contents are freed at once.
//
// The memtable is the reason this exists. It allocates one small node per
// entry, never frees an individual node, and is discarded wholesale after being
// flushed to an SSTable. General-purpose allocation is the wrong shape for that
// in two ways:
//
//   * Cost. A skiplist node is tens of bytes; a malloc header plus rounding is
//     a large fraction of that, and freeing a million nodes one at a time at
//     flush is time spent walking a structure that is about to disappear.
//   * Accounting. Flush is triggered by memtable size, so the engine must know
//     how much memory a memtable actually occupies. MemoryUsage() reports the
//     bytes reserved from the operating system, not an estimate summed from
//     entry sizes that ignores per-allocation overhead.
//
// Not thread-safe. A memtable has exactly one writer, which is what makes the
// unsynchronised bump pointer sound; readers never allocate.
class Arena {
 public:
  Arena() = default;
  ~Arena() = default;

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) noexcept = default;
  Arena& operator=(Arena&&) noexcept = default;

  // Returns at least `bytes` of storage with no alignment guarantee beyond 1.
  // The returned memory is uninitialised and stays valid until the Arena dies.
  [[nodiscard]] char* Allocate(std::size_t bytes);

  // As Allocate, aligned to at least alignof(std::max_align_t). Use this for
  // anything that will hold a pointer or an atomic.
  [[nodiscard]] char* AllocateAligned(std::size_t bytes);

  // Total bytes reserved from the allocator, including the space still unused
  // in the current block and the bookkeeping for the block list. This is the
  // number a flush threshold should be compared against.
  [[nodiscard]] std::size_t MemoryUsage() const noexcept { return memory_usage_; }

  // Bytes handed out to callers. Always <= MemoryUsage(); the difference is
  // internal fragmentation, which is worth being able to observe.
  [[nodiscard]] std::size_t BytesAllocated() const noexcept { return bytes_allocated_; }

 private:
  // Blocks are 4 KiB so a fresh Arena costs one page. Requests larger than a
  // quarter of that get a block of their own rather than wasting the remainder
  // of the current one.
  static constexpr std::size_t kBlockSize = 4096;
  static constexpr std::size_t kLargeRequestThreshold = kBlockSize / 4;

  char* AllocateFallback(std::size_t bytes);
  char* AllocateNewBlock(std::size_t block_bytes);

  char* cursor_ = nullptr;
  std::size_t remaining_ = 0;
  std::size_t memory_usage_ = 0;
  std::size_t bytes_allocated_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
};

}  // namespace strata

#include "util/arena.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace strata {
namespace {

// The alignment AllocateAligned promises. Enough for any scalar the engine
// stores in arena memory, including the atomic pointers in a skiplist node.
constexpr std::size_t kAlignment = alignof(std::max_align_t);

static_assert((kAlignment & (kAlignment - 1)) == 0, "alignment must be a power of two");

}  // namespace

char* Arena::Allocate(std::size_t bytes) {
  // Zero-byte allocations would return a pointer callers could compare equal to
  // a neighbouring allocation, which makes ownership bugs hard to see.
  assert(bytes > 0 && "Arena::Allocate(0) is meaningless");

  if (bytes <= remaining_) {
    char* const result = cursor_;
    cursor_ += bytes;
    remaining_ -= bytes;
    bytes_allocated_ += bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

char* Arena::AllocateAligned(std::size_t bytes) {
  assert(bytes > 0 && "Arena::AllocateAligned(0) is meaningless");

  const auto current = reinterpret_cast<std::uintptr_t>(cursor_);
  const std::size_t misalignment = static_cast<std::size_t>(current) & (kAlignment - 1);
  const std::size_t padding = misalignment == 0 ? 0 : kAlignment - misalignment;

  if (bytes + padding <= remaining_) {
    char* const result = cursor_ + padding;
    cursor_ += bytes + padding;
    remaining_ -= bytes + padding;
    bytes_allocated_ += bytes;
    assert((reinterpret_cast<std::uintptr_t>(result) & (kAlignment - 1)) == 0);
    return result;
  }

  // A fresh block comes from operator new[], which is already suitably aligned
  // for any type, so the fallback needs no padding of its own.
  char* const result = AllocateFallback(bytes);
  assert((reinterpret_cast<std::uintptr_t>(result) & (kAlignment - 1)) == 0);
  return result;
}

char* Arena::AllocateFallback(std::size_t bytes) {
  if (bytes > kLargeRequestThreshold) {
    // Give a large request its own exactly-sized block rather than starting a
    // new shared block and abandoning most of the old one.
    bytes_allocated_ += bytes;
    return AllocateNewBlock(bytes);
  }

  // Abandon whatever is left of the current block. At most
  // kLargeRequestThreshold bytes are wasted, which bounds fragmentation.
  cursor_ = AllocateNewBlock(kBlockSize);
  remaining_ = kBlockSize;

  char* const result = cursor_;
  cursor_ += bytes;
  remaining_ -= bytes;
  bytes_allocated_ += bytes;
  return result;
}

char* Arena::AllocateNewBlock(std::size_t block_bytes) {
  blocks_.push_back(std::make_unique<char[]>(block_bytes));
  // Counts the block plus the unique_ptr tracking it, so MemoryUsage() reflects
  // what the process actually holds rather than only the payload.
  memory_usage_ += block_bytes + sizeof(std::unique_ptr<char[]>);
  return blocks_.back().get();
}

}  // namespace strata

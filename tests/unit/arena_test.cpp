#include "util/arena.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <set>
#include <vector>

#include <gtest/gtest.h>

namespace {

using strata::Arena;

// Arena::Allocate is [[nodiscard]] because dropping the pointer leaks the
// region for the life of the Arena. Tests that only care about the accounting
// discard it deliberately, and say so here rather than at nine call sites.
void Reserve(Arena& arena, std::size_t bytes) {
  static_cast<void>(arena.Allocate(bytes));
}

TEST(Arena, StartsEmpty) {
  const Arena arena;
  EXPECT_EQ(arena.MemoryUsage(), 0U);
  EXPECT_EQ(arena.BytesAllocated(), 0U);
}

TEST(Arena, ReturnsUsableDistinctRegions) {
  Arena arena;
  char* const first = arena.Allocate(16);
  char* const second = arena.Allocate(16);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_GE(second, first + 16);

  std::memset(first, 0xAB, 16);
  std::memset(second, 0xCD, 16);
  EXPECT_EQ(static_cast<unsigned char>(first[15]), 0xABU);
  EXPECT_EQ(static_cast<unsigned char>(second[0]), 0xCDU);
}

TEST(Arena, ReservesAWholeBlockOnFirstAllocation) {
  Arena arena;
  Reserve(arena, 1);
  // One 4 KiB block, not one byte: the point of the Arena is that the process
  // pays for pages, and MemoryUsage must report that rather than the payload.
  EXPECT_GE(arena.MemoryUsage(), 4096U);
  EXPECT_EQ(arena.BytesAllocated(), 1U);
}

TEST(Arena, DoesNotGrowWhileABlockHasRoom) {
  Arena arena;
  Reserve(arena, 64);
  const std::size_t after_first = arena.MemoryUsage();
  for (int index = 0; index < 8; ++index) {
    Reserve(arena, 64);
  }
  EXPECT_EQ(arena.MemoryUsage(), after_first);
  EXPECT_EQ(arena.BytesAllocated(), 64U * 9U);
}

TEST(Arena, GrowsWhenTheCurrentBlockIsExhausted) {
  Arena arena;
  const std::size_t before = arena.MemoryUsage();
  for (int index = 0; index < 200; ++index) {
    Reserve(arena, 64);
  }
  EXPECT_GT(arena.MemoryUsage(), before + 4096U);
}

TEST(Arena, GivesALargeRequestItsOwnBlock) {
  Arena arena;
  Reserve(arena, 32);
  const std::size_t after_small = arena.MemoryUsage();

  constexpr std::size_t kLarge = 64 * 1024;
  char* const large = arena.Allocate(kLarge);
  ASSERT_NE(large, nullptr);
  std::memset(large, 0, kLarge);  // must be fully writable

  // Roughly the request itself, not another shared block on top of it.
  EXPECT_GE(arena.MemoryUsage(), after_small + kLarge);
  EXPECT_LT(arena.MemoryUsage(), after_small + kLarge + 4096U);
}

TEST(Arena, SmallAllocationsStillFitAfterALargeOne) {
  Arena arena;
  Reserve(arena, 16);
  Reserve(arena, 64 * 1024);
  char* const after = arena.Allocate(16);
  ASSERT_NE(after, nullptr);
  std::memset(after, 1, 16);
}

TEST(Arena, AlignedAllocationsAreAligned) {
  Arena arena;
  // Interleave odd-sized unaligned requests so the cursor is left misaligned
  // before each aligned one, which is the case that actually needs padding.
  for (std::size_t size = 1; size <= 64; ++size) {
    Reserve(arena, size);
    char* const aligned = arena.AllocateAligned(size);
    ASSERT_NE(aligned, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned) % alignof(std::max_align_t), 0U)
        << "size " << size;
  }
}

TEST(Arena, AlignedAllocationsAreAlignedAcrossBlockBoundaries) {
  Arena arena;
  for (int index = 0; index < 500; ++index) {
    char* const aligned = arena.AllocateAligned(37);
    ASSERT_NE(aligned, nullptr);
    ASSERT_EQ(reinterpret_cast<std::uintptr_t>(aligned) % alignof(std::max_align_t), 0U)
        << "iteration " << index;
    std::memset(aligned, index & 0xFF, 37);
  }
}

TEST(Arena, RegionsNeverOverlap) {
  // Writes a distinct byte pattern into every region, then re-reads them all.
  // Overlapping regions would show up as a pattern overwritten by a later one.
  Arena arena;
  std::mt19937 random(1234);
  std::uniform_int_distribution<std::size_t> sizes(1, 3000);

  std::vector<std::pair<char*, std::size_t>> regions;
  regions.reserve(400);
  for (int index = 0; index < 400; ++index) {
    const std::size_t size = sizes(random);
    char* const region = (index % 3 == 0) ? arena.AllocateAligned(size) : arena.Allocate(size);
    ASSERT_NE(region, nullptr);
    std::memset(region, index & 0xFF, size);
    regions.emplace_back(region, size);
  }

  for (std::size_t index = 0; index < regions.size(); ++index) {
    const auto [region, size] = regions[index];
    const auto expected = static_cast<unsigned char>(index & 0xFF);
    for (std::size_t offset = 0; offset < size; ++offset) {
      ASSERT_EQ(static_cast<unsigned char>(region[offset]), expected)
          << "region " << index << " byte " << offset;
    }
  }
}

TEST(Arena, BytesAllocatedNeverExceedsMemoryUsage) {
  Arena arena;
  std::mt19937 random(99);
  std::uniform_int_distribution<std::size_t> sizes(1, 5000);
  for (int index = 0; index < 300; ++index) {
    Reserve(arena, sizes(random));
    ASSERT_LE(arena.BytesAllocated(), arena.MemoryUsage());
  }
}

}  // namespace

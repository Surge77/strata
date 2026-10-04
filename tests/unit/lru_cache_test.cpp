#include "util/lru_cache.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Cache = strata::LruCache<int, std::string>;

std::shared_ptr<const std::string> Value(const std::string& text) {
  return std::make_shared<const std::string>(text);
}

// Dereferencing a handle directly trips -Wnull-dereference; a miss reads as
// "<null>" instead.
std::string Text(const Cache::Handle& handle) {
  return handle != nullptr ? *handle : "<null>";
}

TEST(LruCacheTest, LookupMissesUntilInserted) {
  Cache cache(4);
  EXPECT_EQ(cache.Lookup(1), nullptr);
  cache.Insert(1, Value("one"), 1);
  EXPECT_EQ(Text(cache.Lookup(1)), "one");
}

TEST(LruCacheTest, EvictsTheLeastRecentlyUsedEntryWhenFull) {
  Cache cache(2);
  cache.Insert(1, Value("one"), 1);
  cache.Insert(2, Value("two"), 1);
  cache.Insert(3, Value("three"), 1);
  EXPECT_EQ(cache.Lookup(1), nullptr);
  EXPECT_NE(cache.Lookup(2), nullptr);
  EXPECT_NE(cache.Lookup(3), nullptr);
  EXPECT_EQ(cache.usage(), 2U);
}

TEST(LruCacheTest, LookupMakesAnEntryRecentlyUsed) {
  Cache cache(2);
  cache.Insert(1, Value("one"), 1);
  cache.Insert(2, Value("two"), 1);
  ASSERT_NE(cache.Lookup(1), nullptr);
  cache.Insert(3, Value("three"), 1);
  EXPECT_NE(cache.Lookup(1), nullptr);
  EXPECT_EQ(cache.Lookup(2), nullptr);
}

TEST(LruCacheTest, EvictsByChargeNotByCount) {
  Cache cache(10);
  cache.Insert(1, Value("small"), 3);
  cache.Insert(2, Value("small"), 3);
  cache.Insert(3, Value("small"), 3);
  cache.Insert(4, Value("large"), 8);
  EXPECT_EQ(cache.Lookup(1), nullptr);
  EXPECT_EQ(cache.Lookup(2), nullptr);
  EXPECT_EQ(cache.Lookup(3), nullptr) << "one large entry displaces three small ones";
  EXPECT_NE(cache.Lookup(4), nullptr);
  EXPECT_EQ(cache.usage(), 8U);
}

TEST(LruCacheTest, EntryLargerThanCapacityIsReturnedButNotCached) {
  Cache cache(4);
  cache.Insert(1, Value("kept"), 2);
  const auto returned = cache.Insert(2, Value("too large"), 5);
  EXPECT_EQ(Text(returned), "too large");
  EXPECT_EQ(cache.Lookup(2), nullptr);
  EXPECT_NE(cache.Lookup(1), nullptr) << "an uncacheable entry must not evict others";
}

TEST(LruCacheTest, ZeroCapacityCachesNothing) {
  Cache cache(0);
  EXPECT_NE(cache.Insert(1, Value("one"), 1), nullptr);
  EXPECT_EQ(cache.Lookup(1), nullptr);
  EXPECT_EQ(cache.usage(), 0U);
}

TEST(LruCacheTest, ExistingEntryWinsADuplicateInsert) {
  Cache cache(4);
  cache.Insert(1, Value("first"), 1);
  const auto returned = cache.Insert(1, Value("second"), 1);
  EXPECT_EQ(Text(returned), "first");
  EXPECT_EQ(Text(cache.Lookup(1)), "first");
  EXPECT_EQ(cache.usage(), 1U);
}

TEST(LruCacheTest, EvictedValueStaysAliveWhileHeld) {
  Cache cache(1);
  cache.Insert(1, Value("held"), 1);
  const auto held = cache.Lookup(1);
  cache.Insert(2, Value("evicts it"), 1);
  ASSERT_EQ(cache.Lookup(1), nullptr);
  EXPECT_EQ(Text(held), "held");
}

TEST(LruCacheTest, EraseRemovesAnEntryAndItsCharge) {
  Cache cache(4);
  cache.Insert(1, Value("one"), 3);
  cache.Erase(1);
  cache.Erase(42);
  EXPECT_EQ(cache.Lookup(1), nullptr);
  EXPECT_EQ(cache.usage(), 0U);
}

TEST(LruCacheTest, ConcurrentUseStaysWithinCapacity) {
  constexpr std::size_t kCapacity = 16;
  constexpr int kThreads = 4;
  constexpr int kOperations = 2000;
  Cache cache(kCapacity);
  std::vector<std::thread> threads;
  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&cache, thread] {
      for (int op = 0; op < kOperations; ++op) {
        const int key = (op * 7 + thread) % 64;
        auto handle = cache.Lookup(key);
        if (handle == nullptr) {
          handle = cache.Insert(key, Value(std::to_string(key)), 1);
        }
        // A cache that mixed up entries under contention would fail here.
        EXPECT_EQ(Text(handle), std::to_string(key));
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_LE(cache.usage(), kCapacity);
}

}  // namespace

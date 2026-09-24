#include "db/skiplist.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <numeric>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "util/arena.hpp"

namespace {

using strata::Arena;

struct Compare {
  int operator()(const std::uint64_t& a, const std::uint64_t& b) const {
    return a < b ? -1 : (a > b ? 1 : 0);
  }
};

using List = strata::SkipList<std::uint64_t, Compare>;

TEST(SkipList, EmptyListHasNothing) {
  Arena arena;
  const List list(Compare{}, &arena);
  EXPECT_FALSE(list.Contains(10));
  List::Iterator it(&list);
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  it.SeekToLast();
  EXPECT_FALSE(it.Valid());
  it.Seek(100);
  EXPECT_FALSE(it.Valid());
}

// Differential test: every observable operation must agree with std::set.
TEST(SkipList, AgreesWithStdSet) {
  constexpr std::uint64_t kRange = 5000;
  std::mt19937_64 random(1000);
  Arena arena;
  List list(Compare{}, &arena);
  std::set<std::uint64_t> model;
  for (int index = 0; index < 2000; ++index) {
    const std::uint64_t key = random() % kRange;
    if (model.insert(key).second) {
      list.Insert(key);
    }
  }

  for (std::uint64_t key = 0; key < kRange; ++key) {
    ASSERT_EQ(list.Contains(key), model.contains(key)) << key;
  }

  List::Iterator it(&list);
  it.SeekToFirst();
  for (const std::uint64_t expected : model) {
    ASSERT_TRUE(it.Valid());
    ASSERT_EQ(it.key(), expected);
    it.Next();
  }
  EXPECT_FALSE(it.Valid());

  it.SeekToLast();
  for (auto expected = model.rbegin(); expected != model.rend(); ++expected) {
    ASSERT_TRUE(it.Valid());
    ASSERT_EQ(it.key(), *expected);
    it.Prev();
  }
  EXPECT_FALSE(it.Valid());

  for (std::uint64_t target = 0; target <= kRange; target += 7) {
    it.Seek(target);
    const auto expected = model.lower_bound(target);
    if (expected == model.end()) {
      EXPECT_FALSE(it.Valid()) << target;
    } else {
      ASSERT_TRUE(it.Valid()) << target;
      EXPECT_EQ(it.key(), *expected) << target;
    }
  }
}

TEST(SkipList, HandlesAscendingAndDescendingInsertion) {
  Arena arena;
  List list(Compare{}, &arena);
  for (std::uint64_t key = 1000; key < 2000; ++key) {
    list.Insert(key);
  }
  for (std::uint64_t key = 999; key > 0; --key) {
    list.Insert(key);
  }
  List::Iterator it(&list);
  it.SeekToFirst();
  for (std::uint64_t expected = 1; expected < 2000; ++expected) {
    ASSERT_TRUE(it.Valid());
    ASSERT_EQ(it.key(), expected);
    it.Next();
  }
  EXPECT_FALSE(it.Valid());
}

// One writer inserts a known permutation and publishes how many keys it has
// inserted. Readers, with no lock, must always find every published key and
// always see strictly ascending order. Under ThreadSanitizer this also checks
// that the acquire/release ordering on the links is sufficient.
TEST(SkipList, ReadersNeedNoLockWhileOneWriterInserts) {
  constexpr std::uint64_t kKeys = 20000;
  constexpr int kReaders = 4;
  std::vector<std::uint64_t> order(kKeys);
  std::iota(order.begin(), order.end(), 0);
  std::shuffle(order.begin(), order.end(), std::mt19937_64(7));

  Arena arena;
  List list(Compare{}, &arena);
  std::atomic<std::size_t> published{0};
  std::atomic<bool> failed{false};
  std::atomic<int> scans_during_writes{0};

  std::vector<std::thread> readers;
  for (int reader = 0; reader < kReaders; ++reader) {
    readers.emplace_back([&, reader] {
      std::mt19937_64 random(static_cast<std::uint64_t>(reader));
      while (published.load(std::memory_order_acquire) < kKeys && !failed.load()) {
        const std::size_t visible = published.load(std::memory_order_acquire);
        // Spot-check published keys: each must already be reachable.
        for (int probe = 0; probe < 20 && visible > 0; ++probe) {
          if (!list.Contains(order[random() % visible])) {
            failed.store(true);
          }
        }
        // A full scan must be strictly ascending and include at least every
        // key published before the scan began.
        std::size_t seen = 0;
        std::uint64_t previous = 0;
        List::Iterator it(&list);
        for (it.SeekToFirst(); it.Valid(); it.Next()) {
          if (seen > 0 && it.key() <= previous) {
            failed.store(true);
          }
          previous = it.key();
          ++seen;
        }
        if (seen < visible) {
          failed.store(true);
        }
        scans_during_writes.fetch_add(1);
      }
    });
  }

  for (std::size_t index = 0; index < kKeys; ++index) {
    list.Insert(order[index]);
    published.store(index + 1, std::memory_order_release);
    if (index % 64 == 0) {
      std::this_thread::yield();  // let readers interleave with the writer
    }
  }
  for (auto& thread : readers) {
    thread.join();
  }
  EXPECT_FALSE(failed.load());
  // Guard against a vacuous pass: the readers must actually have overlapped
  // with the writer.
  EXPECT_GT(scans_during_writes.load(), kReaders);
}

}  // namespace

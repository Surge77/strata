#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace strata {

// What a cache has done and holds, for the stats API.
struct LruCacheStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::size_t usage = 0;
  std::size_t capacity = 0;
  std::size_t entries = 0;
};

// A thread-safe, capacity-bounded cache that evicts the least recently used
// entries first. Each entry has a caller-chosen charge (1 for "one open
// table", the byte size for a block); the cache keeps the sum of charges at
// or below its capacity.
//
// Values are shared_ptrs, so eviction only drops the cache's reference: a
// reader still holding a value keeps it alive. That is what lets the table
// cache close tables while a lookup is still reading one.
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class LruCache {
 public:
  using Handle = std::shared_ptr<const Value>;

  explicit LruCache(std::size_t capacity) : capacity_(capacity) {}

  LruCache(const LruCache&) = delete;
  LruCache& operator=(const LruCache&) = delete;
  LruCache(LruCache&&) = delete;
  LruCache& operator=(LruCache&&) = delete;
  ~LruCache() = default;

  // The cached value, now the most recently used; null on a miss.
  [[nodiscard]] Handle Lookup(const Key& key) {
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto found = index_.find(key);
    if (found == index_.end()) {
      ++misses_;
      return nullptr;
    }
    ++hits_;
    entries_.splice(entries_.begin(), entries_, found->second);
    return found->second->value;
  }

  // Caches `value` and returns the value now cached under `key`. If another
  // thread inserted the key first, its value wins and is returned, so racing
  // loaders agree on one copy. A value whose charge exceeds the capacity is
  // returned without being cached rather than flushing everything else.
  Handle Insert(const Key& key, Handle value, std::size_t charge) {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (const auto found = index_.find(key); found != index_.end()) {
      entries_.splice(entries_.begin(), entries_, found->second);
      return found->second->value;
    }
    if (charge > capacity_) {
      return value;
    }
    entries_.push_front(Entry{key, value, charge});
    try {
      index_.emplace(key, entries_.begin());
    } catch (...) {
      // An entry in the list but not the index would later be evicted with a
      // charge usage_ never counted, wrapping it around.
      entries_.pop_front();
      throw;
    }
    usage_ += charge;
    while (usage_ > capacity_) {
      RemoveLocked(std::prev(entries_.end()));
    }
    return value;
  }

  void Erase(const Key& key) {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (const auto found = index_.find(key); found != index_.end()) {
      RemoveLocked(found->second);
    }
  }

  [[nodiscard]] std::size_t usage() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return usage_;
  }

  // Counted under the lock every Lookup already takes, so the hot path pays
  // two increments and nothing else.
  [[nodiscard]] LruCacheStats Stats() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return LruCacheStats{.hits = hits_,
                         .misses = misses_,
                         .usage = usage_,
                         .capacity = capacity_,
                         .entries = index_.size()};
  }

 private:
  struct Entry {
    Key key;
    Handle value;
    std::size_t charge;
  };
  using EntryList = std::list<Entry>;

  void RemoveLocked(typename EntryList::iterator entry) {
    usage_ -= entry->charge;
    index_.erase(entry->key);
    entries_.erase(entry);
  }

  const std::size_t capacity_;
  mutable std::mutex mutex_;
  EntryList entries_;  // most recently used first
  std::unordered_map<Key, typename EntryList::iterator, Hash> index_;
  std::size_t usage_ = 0;
  std::uint64_t hits_ = 0;
  std::uint64_t misses_ = 0;
};

}  // namespace strata

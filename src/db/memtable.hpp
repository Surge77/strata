#pragma once

#include "strata/iterator.hpp"
#include "strata/slice.hpp"
#include "strata/status.hpp"

#include <cstddef>
#include <memory>
#include <string>

#include "db/dbformat.hpp"
#include "db/skiplist.hpp"
#include "util/arena.hpp"

namespace strata {

// The in-memory, sorted home of recent writes.
//
// Every write lands here after it is in the WAL. Entries are never updated
// or removed in place: a new Put or Delete for a key adds a newer entry, and
// reads pick the newest one visible at their sequence number. When the
// memtable grows past its size limit it is frozen and flushed to an SSTable
// (phase 5).
//
// Each entry is one Arena allocation:
//
//   varint32 internal_key_size | user_key | fixed64(seq << 8 | type)
//   varint32 value_size        | value
//
// Thread safety: Add requires external serialisation (the DB's write lock);
// Get and iterators may run concurrently with Add and with each other.
class MemTable {
 public:
  // `comparator` must outlive the memtable.
  explicit MemTable(const InternalKeyComparator* comparator);

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;
  MemTable(MemTable&&) = delete;
  MemTable& operator=(MemTable&&) = delete;
  ~MemTable() = default;

  // Bytes reserved for this memtable's data. Compared against the flush
  // threshold; it includes allocator overhead, not just keys and values.
  [[nodiscard]] std::size_t ApproximateMemoryUsage() const noexcept { return arena_.MemoryUsage(); }

  // True if nothing has been added. An empty memtable still uses memory (its
  // arena's first block), so this is not ApproximateMemoryUsage() == 0.
  [[nodiscard]] bool empty() const;

  // For a deletion, `value` is ignored (and should be empty).
  void Add(SequenceNumber sequence, ValueType type, Slice user_key, Slice value);

  // The result of looking a key up in this memtable alone.
  enum class LookupResult : std::uint8_t {
    kNotPresent,  // no version at or below the sequence: look in older data
    kFound,       // the newest visible version is a value (copied to *value)
    kDeleted,     // the newest visible version is a tombstone: stop searching
  };

  // Finds the newest version of key.user_key() with a sequence number at or
  // below key's. kDeleted and kNotPresent differ in a way that matters to the
  // read path: a tombstone here hides every older version in older files.
  [[nodiscard]] LookupResult Get(const LookupKey& key, std::string* value) const;

  // Iterates over every entry in internal-key order: user key ascending, then
  // newest version first. key() returns the internal key.
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator() const;

 private:
  class MemTableIterator;

  // Orders arena entries (pointers to the length-prefixed internal key).
  struct KeyComparator {
    const InternalKeyComparator* comparator;
    int operator()(const char* a, const char* b) const;
  };
  using Table = SkipList<const char*, KeyComparator>;

  Arena arena_;
  Table table_;
};

}  // namespace strata

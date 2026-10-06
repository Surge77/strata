#pragma once

#include "strata/iterator.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "db/dbformat.hpp"
#include "util/comparator.hpp"

namespace strata {

// Optional limits on the user keys a scan visits: `lower` inclusive, `upper`
// exclusive.
struct DBIterBounds {
  std::optional<std::string> lower;
  std::optional<std::string> upper;
};

// Turns a stream of internal entries -- every version of every key, newest
// first within a key, tombstones included -- into the user's view as of
// `sequence`: one entry per key, holding its newest value at or below that
// sequence number, with deleted keys left out. Works in both directions.
//
// A malformed internal key ends the scan: Valid() becomes false and status()
// is Corruption, rather than skipping entries the user would never know
// were missing. `keep_alive` is held for the iterator's lifetime, for
// whatever the internal iterator reads from (memtables, a Version, caches).
std::unique_ptr<Iterator> NewDBIterator(const Comparator* user_comparator,
                                        std::unique_ptr<Iterator> internal, SequenceNumber sequence,
                                        DBIterBounds bounds,
                                        std::vector<std::shared_ptr<const void>> keep_alive);

}  // namespace strata

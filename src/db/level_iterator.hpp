#pragma once

#include "strata/iterator.hpp"

#include <memory>
#include <vector>

#include "db/dbformat.hpp"
#include "db/table_cache.hpp"
#include "db/version_edit.hpp"

namespace strata {

// Iterates the tables of one level (1 or deeper) as a single sorted stream.
// Those tables are sorted and disjoint, so this concatenates them rather than
// merging, and opens each one only when the scan reaches it.
//
// A table that cannot be opened or read ends the iteration: Valid() becomes
// false and status() reports why. `tables` and `files` (a Version's list,
// pinned by the caller) must outlive the iterator; the list is not copied,
// so creating an iterator costs nothing per table.
std::unique_ptr<Iterator> NewLevelIterator(const InternalKeyComparator* comparator,
                                           TableCache* tables,
                                           const std::vector<FileMetaData>* files);

}  // namespace strata

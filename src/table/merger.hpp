#pragma once

#include "strata/iterator.hpp"

#include <memory>
#include <vector>

#include "util/comparator.hpp"

namespace strata::table {

// Presents several sorted iterators as one sorted stream, in both directions.
// Children must not share keys -- internal keys never do, because each carries
// a unique sequence number.
//
// A child that fails ends the merge: the iterator becomes !Valid() and
// status() reports the failure. Skipping the child and carrying on would hand
// compaction a stream missing that child's remaining entries, and it would
// write tables without them.
std::unique_ptr<Iterator> NewMergingIterator(const Comparator* comparator,
                                             std::vector<std::unique_ptr<Iterator>> children);

}  // namespace strata::table

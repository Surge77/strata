#include "table/block_builder.hpp"

#include <algorithm>
#include <cassert>

#include "util/coding.hpp"

namespace strata::table {

BlockBuilder::BlockBuilder(const Comparator* comparator, int restart_interval)
    : comparator_(comparator), restart_interval_(restart_interval) {
  assert(restart_interval_ >= 1);
  Reset();
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);  // the first entry is always a restart point
  entries_since_restart_ = 0;
  finished_ = false;
  last_key_.clear();
}

std::size_t BlockBuilder::CurrentSizeEstimate() const noexcept {
  return buffer_.size() + restarts_.size() * sizeof(std::uint32_t) + sizeof(std::uint32_t);
}

void BlockBuilder::Add(Slice key, Slice value) {
  assert(!finished_);
  assert(buffer_.empty() || comparator_->Compare(key, last_key_) > 0);

  std::size_t shared = 0;
  if (entries_since_restart_ < restart_interval_) {
    shared = SharedPrefixLength(last_key_, key);
  } else {
    // A restart point stores its key in full, so a seek can start here.
    restarts_.push_back(static_cast<std::uint32_t>(buffer_.size()));
    entries_since_restart_ = 0;
  }
  const std::size_t non_shared = key.size() - shared;

  PutVarint32(&buffer_, static_cast<std::uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<std::uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<std::uint32_t>(value.size()));
  buffer_.append(key.substr(shared));
  buffer_.append(value);

  last_key_.resize(shared);
  last_key_.append(key.substr(shared));
  ++entries_since_restart_;
}

Slice BlockBuilder::Finish() {
  for (const std::uint32_t restart : restarts_) {
    PutFixed32(&buffer_, restart);
  }
  PutFixed32(&buffer_, static_cast<std::uint32_t>(restarts_.size()));
  finished_ = true;
  return buffer_;
}

}  // namespace strata::table

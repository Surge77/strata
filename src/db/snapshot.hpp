#pragma once

#include "strata/snapshot.hpp"

#include <algorithm>
#include <list>

#include "db/dbformat.hpp"

namespace strata {

class SnapshotImpl final : public Snapshot {
 public:
  explicit SnapshotImpl(SequenceNumber sequence) : sequence_(sequence) {}

  [[nodiscard]] SequenceNumber sequence() const noexcept { return sequence_; }

 private:
  const SequenceNumber sequence_;
};

// The live snapshots, oldest first. Snapshots are taken at the latest
// sequence number, which never decreases, so creation order is sequence
// order and the oldest is always at the front.
//
// Not synchronised: the DB calls it with its mutex held. Lookups go by
// address, so a released or foreign pointer is reported rather than trusted.
class SnapshotList {
 public:
  const SnapshotImpl* New(SequenceNumber sequence) { return &snapshots_.emplace_back(sequence); }

  // The snapshot this pointer names, or null if it is not one of ours.
  [[nodiscard]] const SnapshotImpl* Find(const Snapshot* snapshot) const {
    const auto found =
        std::find_if(snapshots_.begin(), snapshots_.end(),
                     [snapshot](const SnapshotImpl& live) { return &live == snapshot; });
    return found == snapshots_.end() ? nullptr : &*found;
  }

  // False if `snapshot` is not live.
  bool Release(const Snapshot* snapshot) {
    const auto found =
        std::find_if(snapshots_.begin(), snapshots_.end(),
                     [snapshot](const SnapshotImpl& live) { return &live == snapshot; });
    if (found == snapshots_.end()) {
      return false;
    }
    snapshots_.erase(found);
    return true;
  }

  [[nodiscard]] bool empty() const noexcept { return snapshots_.empty(); }
  // Precondition: !empty().
  [[nodiscard]] SequenceNumber oldest() const { return snapshots_.front().sequence(); }

 private:
  std::list<SnapshotImpl> snapshots_;
};

}  // namespace strata

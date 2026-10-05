#pragma once

namespace strata {

// A frozen, read-only view of the database at one moment. Reads given a
// snapshot (ReadOptions::snapshot) see exactly what was committed when it was
// taken, however the database changes afterwards.
//
// Obtain one with DB::GetSnapshot and give it back with DB::ReleaseSnapshot.
// While a snapshot is held, compaction keeps the old versions it can see, so
// a long-lived snapshot costs disk space. Release snapshots before closing
// the database: a snapshot does not outlive the DB that made it.
class Snapshot {
 public:
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
  Snapshot(Snapshot&&) = delete;
  Snapshot& operator=(Snapshot&&) = delete;

 protected:
  Snapshot() = default;
  ~Snapshot() = default;
};

}  // namespace strata

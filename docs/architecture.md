# Architecture

Strata is an embedded C++20 key-value engine. `strata::DB` is the public,
move-only handle. `DBImpl` owns the active log and memtables, the background
flush thread, a `VersionSet`, and a `TableCache`. The current implementation
stores new tables at level 0. Compaction and Bloom filters are later phases.

## Write path

`DB::Put`, `Delete`, and `Write` encode a `WriteBatch`. Under the DB mutex,
`DBImpl::Write` assigns sequence numbers, appends one framed record to the
write-ahead log, and syncs the log when `WriteOptions::sync` is true. Only
after the append succeeds does it insert the batch into the active `MemTable`
and acknowledge the write. A default write survives a process crash. A synced
write survives power loss under the filesystem assumptions in
[durability.md](durability.md).

When the active memtable exceeds `write_buffer_size`, `MakeRoomForWrite` starts
a fresh log, retains the old memtable as immutable, and starts a background
flush. Writers wait if the previous immutable memtable has not finished
flushing. A flush writes and syncs an SSTable, syncs its directory, then
commits a `VersionEdit` to the MANIFEST. The edit records the table, the
active log number, and the immutable memtable's last sequence number. Writes
made during the flush stay in the new log and are not marked as flushed.

## Read path

`DBImpl::Get` captures the active memtable, immutable memtable, current
`Version`, and visible sequence number under the mutex. It then releases the
mutex and checks those sources in order. The memtables support concurrent
readers. Level-0 files can overlap, so `Version::Get` searches matching files
from newest to oldest. A deletion marker stops the search and returns
`NotFound`. The held `Version` keeps its table files live while the read runs.
`TableCache` currently keeps opened tables without a size bound; cache limits
are part of phase 6.

## Recovery and publication

`Open` locks the database directory and reads `CURRENT`, which names the live
MANIFEST. `VersionSet::Recover` replays its framed `VersionEdit` records to
reconstruct the file set and counters. The DB then replays log files not yet
covered by the MANIFEST. Real corruption fails open unless WAL salvage was
explicitly requested. A torn tail in the newest log can be discarded.

Recovered entries are flushed to a level-0 table. The new table and new log
number are committed in one MANIFEST edit. Until that commit, the old logs
remain authoritative. `LogAndApply` syncs the edit before making the new
`Version` current. A new session's MANIFEST is first written and synced, then
published through an atomically renamed `CURRENT` and a directory sync.
See [format/manifest.md](format/manifest.md) for the byte format and ordering.

## File lifecycle

The directory contains `LOCK`, `LOG`, numbered `.log` WAL files, numbered
`.sst` tables, `MANIFEST-NNNNNN`, `CURRENT`, and temporary `.dbtmp` files.
The lock prevents two processes from opening the same database. A table is
verified before an edit can reference it. `pending_outputs_` protects a table
being built from garbage collection. After a commit, `DeleteObsoleteFiles`
removes old logs, unreferenced tables, old MANIFESTs, and abandoned temporary
files. It retains tables referenced by any still-live `Version`.

The filesystem and crash tests run on a native Linux filesystem because
directory sync behavior is part of the durability contract.

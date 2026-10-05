# Architecture

Strata is an embedded C++20 key-value engine. `strata::DB` is the public,
move-only handle. `DBImpl` owns the active log and memtables, one background
thread that flushes and compacts, a `VersionSet`, and a `TableCache`. Flushed
tables enter level 0, and compaction moves data down through levels 1-6.

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
`Version`, and visible sequence number under the mutex. With
`ReadOptions::snapshot`, the visible sequence number is the snapshot's instead. It then releases the
mutex and checks those sources in order. The memtables support concurrent
readers. Level-0 files can overlap, so `Version::Get` searches matching files
from newest to oldest. A deletion marker stops the search and returns
`NotFound`. The held `Version` keeps its table files live while the read runs.
New tables use a whole-table Bloom filter to skip data-block reads for definite
misses. Tables created before phase 6 have no filter and use the normal read
path. `Options::bloom_filter` turns the filter off for new tables.

Two LRU caches sit under reads. `TableCache` keeps up to
`Options::max_open_tables` tables open. Each table holds its index and filter
in memory, so a lookup does not reread them. A shared block cache holds up to
`Options::block_cache_size` bytes of parsed, checksum-verified data blocks,
keyed by table file number and block offset. Both caches hand out
`shared_ptr`s, so evicting an entry never frees a table or block that a reader
is still using. A closed table can always be reopened, because obsolete-file
cleanup deletes only files that no live `Version` references.

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

## Compaction

Flush and compaction share one background thread, flush first, so they never
race on the version. Level 0 compacts once it holds 4 tables. The pick is the
oldest table plus every level-0 table overlapping it, transitively, merged
with the overlapping tables of level 1. Deeper levels compact one table at a
time, round-robin, once their bytes pass `max_bytes_for_level_base` x
10^(level-1). A single table with nothing below it is relinked one level down
rather than rewritten.

The merge (`RunCompaction`) streams the inputs through a merging iterator.
That iterator stops at the first read error rather than skipping a table. The
merge drops:
- a version whose newer version is visible to every live snapshot;
- a tombstone that every snapshot sees, when no deeper level could hold an
  older value of its key. Dropping it earlier would bring that value back.

Outputs are cut near `max_file_size`, but never between two versions of one
user key. The tables of a level therefore never share a key, and `ApplyTo`
rejects any version that would break this. Outputs are synced, their
directory synced, and each reopened before the MANIFEST edit that installs
them. A failed compaction deletes its outputs, keeps its inputs, and stops
further writes, as a failed flush does.

Writes are delayed 1 ms (once) when level 0 reaches 8 tables. When it reaches
12, a write that needs a new memtable waits for compaction.

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

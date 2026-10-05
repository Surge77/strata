# Phase 7: Compaction

**Branch:** `feature/phase-07-compaction`. Gate: `CompactionDoesNotResurrectDeletedKey`
passes, and write amplification is measured and reported.

> **Status: in progress.**

## Starting point

Phases 3-6 left a database whose flushes only ever add level-0 tables. Every
lookup that misses the memtables checks every overlapping level-0 table, so
reads slow down as data grows, and overwritten or deleted data is never
reclaimed. `Version::Get` already binary-searches levels 1 and deeper on the
assumption that their files are disjoint; nothing yet puts files there.

## Plan

1. **Merging iterator.** Compaction must read several sorted tables as one
   stream. A merge that skipped a child after a read error would silently drop
   that table's remaining data, so the merge stops at the first child error.
2. **Snapshots.** `DB::GetSnapshot` / `ReleaseSnapshot` and
   `ReadOptions::snapshot`. Compaction may drop an old version of a key only if
   no live snapshot can still see it.
3. **Version invariants.** Applying an edit that makes files in level 1 or
   deeper overlap is rejected as `Corruption`, because lookups rely on it.
4. **Picker and merge job.** One background thread does both flush and
   compaction, so they never race on the version. Flush always goes first.
   Level 0 compacts when it holds 4 files; deeper levels compact when their
   bytes exceed their budget (10x per level). The merge drops a version when a
   newer version of the same key is visible to every snapshot. It drops a
   tombstone only when no snapshot needs it and no deeper level could hold an
   older value of the key. Output files are never split between two versions
   of one user key, so the files of a level never share a user key.
5. **Write stalls.** Writes slow down at 8 level-0 files and stop at 12 until
   compaction catches up.
6. **Write amplification.** Count bytes written by flush and by compaction, and
   report the ratio from a benchmark.

## Decisions

- **One background thread.** Flush and compaction share it, flush first, so
  they never race on the version and no lock ordering is needed between them.
  A long compaction can delay a flush, and writes then wait on the full
  memtable. Phase 9 (concurrency) owns splitting them.
- **Drop rules** (LevelDB's). `smallest_snapshot` is the oldest live snapshot,
  or the newest sequence number if there is none. It is captured when the
  compaction starts. Any snapshot taken later has a higher sequence number,
  so anything hidden from the captured value is hidden from it too.
- **Whole user keys per table.** Outputs are cut only between different user
  keys. Tables in one level therefore never share a key, which is what lets
  `Version::Get` read one table per level. `ApplyTo` now rejects a version
  that breaks this, instead of answering lookups wrongly.
- **Level 0** compacts at 4 tables. The automatic pick is the oldest table
  plus every level-0 table overlapping it, transitively. A table left behind
  shares no key with the chosen ones, so moving them below it is safe.
  Deeper levels compact one table at a time, round-robin, once their bytes
  pass `max_bytes_for_level_base` x 10^(level-1).
- **Trivial moves.** A single input table with nothing overlapping below is
  relinked one level down, not rewritten.
- **Failure.** A failed compaction deletes its outputs, keeps its inputs, and
  stops further writes, as a failed flush already does. A child read error
  stops the merge rather than skipping that table.
- **New options:** `max_file_size` (2 MiB) and `max_bytes_for_level_base`
  (10 MiB). **New API:** `DB::GetSnapshot`, `DB::ReleaseSnapshot`,
  `ReadOptions::snapshot`. A released or foreign snapshot is
  `InvalidArgument`, never trusted.

## Gate: CompactionDoesNotResurrectDeletedKey

The test puts a value into level 2. It then compacts a tombstone for that key
into level 1 through a real merge; an overlapping level-1 table is seeded so
the step is not a trivial move. It checks the key stays deleted before and
after reopening, and again after the tombstone and value meet and both
vanish. **Negative control:** forcing `IsBaseLevelForKey` to return true makes
the test fail, with the deleted value returned at all three checks.

## Gate: write amplification

`benchmarks/write_amp_bench`, 1,000,000 writes of 100-byte values with 19-byte
keys and default options. The last memtable is flushed and background
compaction is waited out before reporting. WA is table bytes (flush +
compaction) per byte of key + value payload. It excludes the write-ahead log
(about 1 more) and MANIFEST records. Tables prefix-compress keys, which is
why a sequential load lands just under 1. Intel Core i5-12450H, WSL2 kernel
6.6.114.1, g++ 13.3.0 `-O2`, ext4, 2026-10-06.

| Workload | Payload MiB | Flush MiB | Compaction MiB | WA | Compactions | Moves |
|---|---:|---:|---:|---:|---:|---:|
| fillseq | 113.5 | 111.1 | 0.0 | 0.98 | 0 | 71 |
| fillrandom | 113.5 | 112.2 | 432.5 | 4.80 | 48 | 14 |
| overwrite (1/4 key space) | 113.5 | 111.6 | 213.0 | 2.86 | 27 | 9 |

- The first run measured **1.74 for a sequential load**, because every
  level-0 compaction merged all of level 0. Taking only the overlapping part
  removed every compaction write for that workload: all 71 tables were moved,
  and WA fell to 0.98.
- Random fill writes each byte about 5 times into tables. That is normal for
  leveled compaction at this size and grows by roughly one rewrite per level
  as data grows. Overwrites cost less, because the merge drops replaced
  versions.
- Random fill stalled about 300 writes (1 ms slowdowns). Level 0 reached 8
  tables while level 1 compacted, which is the stall doing its job.
- Write throughput (not part of the gate) was 290k-720k writes/s and varied up
  to 2x between identical runs on this machine. The byte counts are
  deterministic. Throughput needs the phase 11 harness before it is quoted.
- The review pointed out that an earlier version of this table divided by
  encoded batch bytes, which include batch headers, rather than payload bytes.
  That understated WA by about 12%.

## What went wrong

1. **A livelock that predates this phase.** The write-stall test hung
   without ever flushing. An empty memtable's arena already exceeds a 4 KiB
   `write_buffer_size`. Every write therefore retired an empty memtable,
   flushed nothing, and retired the next one, forever. An empty memtable now
   always takes the write (`MemTable::empty`). A 1-byte write buffer
   regression test covers it.
2. **A negative control that proved nothing.** The first attempt "passed"
   with the rule broken. Those shell commands had left out the toolchain
   `PATH`, so `cmake` was not found, nothing was rebuilt, and a
   `grep "error:"` filter hid the message. The real run fails as it should.
   Build commands now always export the toolchain path, and a build step's
   output is checked as well as filtered.
3. **Tests that tested a move, not a merge.** A single table with nothing
   below it is relinked, not merged. Several tests, including the gate, would
   have exercised the trivial move and never the drop rules. They now seed an
   overlapping table in the target level first.
4. **A phase 6 test broke.** The table-cache test wrote 6 tables and counted
   6 reopens. Background compaction now merges tables once there are 4, so
   the count depended on timing. It now uses 3 tables, still more than its 2
   cache slots.
5. **A test assumption about errors.** The merger test expected one more key
   before a failing child stopped the merge. The child fails as soon as it
   advances, so stopping earlier is the correct and safer behaviour; the test
   was fixed, not the code.
6. **Write amplification 1.74 on a sequential load**, fixed as described
   above.

## Review

An independent C++ review found nothing above MEDIUM and no path that loses or
resurrects data. Fixed:

- **MEDIUM:** a writer stalled at the stop trigger, or behind a flush, waited
  forever if the database closed meanwhile. `Close` now wakes every waiter,
  and stalled writes return `InvalidArgument`. Write-path errors also wake
  waiters. A regression test closes the database under a stalled writer; it
  hung before the fix.
- **MEDIUM:** the first half of the snapshot-retention test ran a trivial
  move, not a merge, so it would have passed with the version-drop rule
  broken. It now seeds an overlapping table and asserts that a compaction
  ran.
- **LOW:**
  - The stall test now waits for the write-stop counter instead of inferring
    the stop from a 300 ms sleep.
  - Outputs written by a compaction whose MANIFEST commit fails are deleted
    immediately.
  - The benchmark counts payload bytes and states what WA excludes.

Accepted:

- **Single-threaded background work.** A long compaction delays a flush. This
  is the stated design until phase 9.
- **`CompactLevelForTest` serialisation.** It is a test hook, and only one
  test thread calls it.
- **Exceptions thrown by a compaction terminate the process.** Allocation
  failure is not recovered elsewhere either.

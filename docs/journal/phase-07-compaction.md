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

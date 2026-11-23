# ADR-0006: One mutex and a group-commit writer queue

**Status:** accepted · **Date:** 2026-10-02 (phase 9), recorded 2026-11-23

## Context

The DB was correct under threads from phase 3 (ADR-0004): one mutex around everything. Phase 9
had to make it fast without giving that up. The two costs that matter are the log write and,
for synced writes, the fsync behind it. A design with finer-grained locks (a lock per
memtable, a lock-free log) multiplies the states a crash test must cover.

## Decision

1. **Writers queue, and the writer at the front leads a group** (LevelDB's design). It merges
   the batches queued behind it, stamps one range of sequence numbers, writes one log record,
   fsyncs once if the group is synced, and inserts into the memtable. Only the leader touches
   the log and the memtable's write side, so they need no lock of their own.
2. **A group becomes visible all at once**: the leader publishes `last_sequence_` under the
   mutex after the memtable insert, so a reader never sees part of a group.
3. **An unsynced leader never carries a synced write**; a synced leader may carry unsynced
   ones (they get more durability than they asked for). Every acknowledged `sync = true`
   write has had its fsync.
4. **The mutex is released only for synced groups.** For an unsynced group the log write is a
   copy into the page cache, and releasing, retaking, and waking followers cost more than
   the copy (8 threads fell from 255k to 57k writes/s when the first version always released
   it).
5. **Reads take the mutex only to pin the current memtables and version**, then search
   without it. WAL rotation happens without the mutex held (the rotation fix after phase 11).

## Consequences

- Synced throughput scales with threads because fsyncs are shared: 1, 2, 4, 8 threads wrote
  about 950, 1,350, 2,250, and 3,850 synced writes/s on the reference laptop (phase 12
  A/B), against roughly 870k unsynced writes/s from one thread.
- There is still one global mutex. A workload that is all small unsynced writes from many
  threads is bounded by it; that is a known limit (`docs/limitations.md`).
- The crash tests stay meaningful: one log writer, one publish point. Removing any of the 11
  sync calls still makes a crash test fail (the negative control).

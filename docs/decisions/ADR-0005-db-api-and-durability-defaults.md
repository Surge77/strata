# ADR-0005: The `DB` API shape, and what a write guarantees by default

**Status:** accepted · **Date:** 2026-09-24

## Context

Phase 3 introduces the public `DB` class. The README has promised its shape since phase 0:

```cpp
strata::DB db;
strata::Status s = strata::DB::Open({.path = "./data"}, &db);
db.Put("user:101", "rahul");
auto value = db.Get("user:101");
db.Delete("user:101");
```

Three decisions are hard to change once users depend on them: how a `DB` object is owned, what
`Get` returns, and whether a write is durable against power loss by default.

## Decision

1. **`DB` is a move-only value type** that owns its implementation through a private pointer.
   `DB::Open(options, &db)` fills in an existing object. Destroying the `DB`, or calling
   `Close()`, flushes and releases the `LOCK`. A default-constructed or moved-from `DB` is
   *closed*, and every operation on it returns `InvalidArgument`, never a crash.
2. **`Get` returns `Result<std::string>`**: the value, or `NotFound` when the key is absent or
   deleted, or another error. This follows ADR-0003. The common "absent" case is a status, not
   an exception or a sentinel value.
3. **`WriteOptions::sync` defaults to `false`.** Every write is appended to the WAL and flushed
   to the operating system before `Put` returns, so it **survives a process crash** (`kill -9`,
   a segfault, an OOM kill). It survives **power loss or a kernel crash only with
   `sync = true`**, which issues `fdatasync` before returning. The two guarantees are documented
   side by side in `docs/durability.md`.
4. **Oversized keys and values are rejected** with `InvalidArgument`: keys longer than
   `2^32 − 9` bytes (so the internal key's length fits a varint32), values longer than
   `2^32 − 1`. They are never truncated. This closes the gap the phase-2 security review
   uncovered.
5. **Recovery fails closed on real corruption.** A torn tail at the end of the WAL is the normal
   result of a crash and is discarded silently. Damage *inside* the log makes `Open` return
   `Corruption`, because otherwise an acknowledged write could be silently dropped.
   `Options::ignore_wal_corruption = true` opts into skipping damaged records (and logging each
   one) for salvage.

## Consequences

**Why not sync by default.** `fdatasync` costs one disk flush per write: milliseconds on an SSD
without power-loss protection, and far more on spinning disks. LevelDB and RocksDB both default to
no sync for this reason. Group commit (phase 9) will spread one sync across many concurrent
writers. That makes `sync = true` much cheaper under load, and the default can be revisited once
it is measured (phase 11).

**What users must know.** A default `Put` that returns `Ok` can be lost if the *machine* loses
power within the next few seconds. It cannot be lost if only the *process* dies. The README and
`docs/durability.md` both say so, and there is a power-loss test for each half of that sentence.

**Why fail closed.** Opening a database with a damaged WAL and quietly skipping records is the
mistake that turns a disk fault into silent data loss. Refusing to open is loud and recoverable.
Skipping is silent and permanent. The operator chooses salvage explicitly.

**Revisit if** measurements in phase 11 show group-committed `sync = true` within a small factor
of `sync = false`, at which point durable-by-default becomes affordable.

## Amendment (phase 3 review, 2026-09-24)

An independent database-internals review found that recovery could produce a state that was
not a prefix of what had been acknowledged: logs replayed after a crash stayed unsynced, and a
later synced write in a newer log could survive a power loss that rolled the older one back.
Three amendments follow:

- **`Close()` fdatasyncs the log.** A clean close makes every acknowledged write durable,
  whatever its `WriteOptions`. Decision 1's "flushes" is replaced by "syncs".
- **Recovery consolidates.** After replay, everything recovered is rewritten into the new log,
  synced, and only then are the replayed logs deleted. Replay skips batches that an
  interrupted consolidation duplicated.
- **Decision 5 is refined.** Damage at the *end of the newest log*, with no valid record after
  it, is a torn write (a `sync = false` power loss can leave zero-filled blocks there), not
  corruption. Damage followed by valid records still fails closed.

The phase-3 consolidation mechanism was superseded in phase 5: recovery now flushes replayed
entries to a level-0 SSTable and commits that table with the new log number in one MANIFEST edit.
The clean-close sync guarantee and WAL corruption policy remain in force.

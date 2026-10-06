# Phase 9: Concurrency

**Branch:** `feature/phase-09-concurrency`. Gate: TSan clean, and a 1/2/4/8
thread scaling curve published.

> **Status: merged into `main` at `0509f64` and tagged `phase-09` (2026-10-06).**

## Starting point

The database was already correct under threads (one mutex, ADR-0004), but
`Write` held that mutex through its log append, its fsync, and its memtable
insert. Writers were fully serialised, each paying its own fsync, and every
reader queued behind whichever fsync was in progress.

## Baseline (before any change)

`benchmarks/concurrency_bench`, 3 rounds, medians. Intel Core i5-12450H
(8 logical CPUs), WSL2 kernel 6.6.114.1, ext4.

| Threads | Unsynced writes/s | Synced writes/s |
|---:|---:|---:|
| 1 | 794,714 | 603 |
| 2 | 266,050 | 455 |
| 4 | 287,433 | 661 |
| 8 | 254,624 | 707 |

| Synced writers | One reader's Gets/s |
|---:|---:|
| 0 | 2,490,444 |
| 1 | 766 |
| 4 | 5 |
| 8 | 1 |

Synced writes did not scale at all, and a reader almost stopped while synced
writers ran: each Get waited for the mutex, which a writer held through its
fsync.

## Decisions

- **Group commit** (LevelDB's design). Writers queue. The front one leads:
  it merges the batches queued behind it, stamps one sequence range, writes
  one log record, fsyncs once if the group is synced, and inserts into the
  memtable. Only the leader touches the log and the memtable's write side,
  so they need no lock of their own. A group's writes become visible only
  when the leader publishes `last_sequence_` under the mutex, so a reader
  never sees part of a group.
- **An unsynced leader never carries a synced write.** It would be
  acknowledged without its fsync. A synced leader may carry unsynced writes;
  they simply get more durability than they asked for.
- **The mutex is released only for synced groups.** The first version released
  it for every group, and unsynced writes got slower: 8 threads fell from
  255k to 57k writes/s, and 1 thread from 795k to 565k. An unsynced group's
  I/O is a copy into the page cache. Dropping and retaking the mutex, and
  waking each follower, cost more than the copy. Only an fsync is slow
  enough to be worth it.
- **A forced memtable switch** (the flush test hook) goes through the writer
  queue as a batch-less writer, so it can never race a leader writing the
  log.
- **`Close` waits for the queue to drain** before closing the log. A leader
  may be writing it with the mutex released.

## Gate: scaling curve (final tree)

Same machine and benchmark, 5 rounds, medians and run-to-run CV.

| Threads | Unsynced writes/s | CV | Synced writes/s | CV |
|---:|---:|---:|---:|---:|
| 1 | 761,286 | 8.5% | 723 | 1.9% |
| 2 | 322,835 | 4.2% | 1,111 | 4.0% |
| 4 | 282,731 | 3.5% | 1,775 | 5.6% |
| 8 | 237,848 | 1.7% | 3,282 | 4.7% |

| Synced writers | One reader's Gets/s | CV |
|---:|---:|---:|
| 0 | 2,667,470 | 1.8% |
| 1 | 2,504,779 | 1.6% |
| 4 | 2,424,109 | 5.5% |
| 8 | 2,367,459 | 0.3% |

- **Synced writes now scale.** At 8 threads they reach 3,282/s, 4.6x the
  baseline's 707, because one fsync serves a whole group.
- **Readers no longer wait for the disk.** With 8 synced writers, a reader
  went from 1 Get/s to 2.37 million: within 11% of an idle database.
- **Unsynced writes do not scale past one thread, and this phase did not
  change that.** They match the baseline (762k at 1 thread, 238k at 8). Every
  write still appends to the one log, under the one mutex, so extra threads
  only add contention. Spreading writes across several logs or memtables
  would fix it, at the cost of the single sequence order recovery relies on.
  That is a design change beyond this phase, and it is recorded here rather
  than hidden.
- The reader benchmark's keys stay in the memtable, so it measures lock
  waiting, not SSTable reads.

## Verification (final tree, `f56e7af`)

- **TSan:** the TSan suite passes **388/388** with no reports. That is the
  gate's "TSan clean"; the plain and ASan/UBSan suites also pass 388/388.
- **Coverage:** **97.4% of lines**, 99.1% of functions. The background-work
  file is at 100%.
- **Format:** the clang-format check is clean.
- **Sync negative control: 11 of 11 caught.** The group commit's log sync
  moved into the leader's path. On its first run that site reported
  `BUILD-FAILED`, not missed: replacing the call left a local pointer unused,
  and `-Werror` refused the mutant. The control now builds with warnings on
  but not fatal, and the site is caught by the flush crash matrix.
- **New crash test:** eight threads write (every third synced) until the
  process dies mid-stream, and power is then lost. Every write covered by an
  acknowledged synced write must survive, including writes fsynced by
  another thread's group leader, and each thread's writes must be a
  gap-free prefix. Five rounds.

## What went wrong

1. **The first version made unsynced writes slower**, as described under
   Decisions. The benchmark written before any change is what caught it.
2. **A test fixture outlived by its own Env.** The gated Env was a local in
   each test, so it was destroyed before the fixture's database, whose final
   `Close` still syncs the log through it. Tests hung at teardown. The Env is
   now a fixture member declared before the database.
3. **My group-count arithmetic.** The test read the counter while the
   leader's own group was still in its fsync, so the right answer is two
   groups (the leader's, and one for all six followers), not one. Without
   group commit it would be seven.
4. **A stray interactive `python -`** in a shell command hung waiting for
   stdin. It was stopped and the edit re-applied from a script file.

## Review

An independent review approved the code, with nothing above LOW. Its MEDIUM
finding was that the gate evidence was not yet committed, which this journal
now records. Fixed:

- **LOW: count overflow.** `BuildBatchGroup` stops before an `Append` that
  would overflow the batch count. It no longer relies on the size cap, which
  would otherwise leave a writer acknowledged but unwritten.
- **LOW: flush hook hang.** The flush test hook also wakes when the database
  closes.
- **LOW: test hangs.** The gated tests wait for queued writers with a
  deadline, and reopen the fsync gate on every exit path, so a failed
  assertion fails the test instead of hanging it.
- **LOW: new tests.**
  - A sync failure for a whole group fails every member and refuses later
    writes.
  - `Close` waits for a leader still in its fsync.
  - Concurrent writes are verified again after reopening.
  - Every write acknowledged before `Close` survives it.

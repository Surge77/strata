# Phase 10: Iterators

**Branch:** `feature/phase-10-iterators`. Gate: scans match the reference model
under concurrent writes.

> **Status: merged into `main` at `11eba51` and tagged `phase-10` (2026-10-06).**

## Starting point

Reads were point lookups only. Phase 7 had built a merging iterator for
compaction. There was no way to walk keys in order, and no public iterator.

## What was built

- **`DBIter`** turns the merged internal stream into the user's view as of a
  sequence number. The stream holds every version of every key, newest first
  within a key, tombstones included. The user sees one entry per key with its
  newest visible value, and deleted keys hidden. It works in both
  directions. The state machine is LevelDB's, extended with an inclusive
  lower bound and an exclusive upper bound.
- **A level iterator** walks the sorted, disjoint tables of level 1 or deeper
  as one stream, and opens each table only when the scan reaches it.
- **`DB::NewIterator(ReadOptions)`** merges the memtable, the immutable
  memtable, every level-0 table, and one level iterator per deeper level. It
  reads as of now or as of `ReadOptions::snapshot`. The new
  `ReadOptions::lower_bound` and `upper_bound` limit it.

## Decisions

- **An iterator is a snapshot in its own right.** It holds the memtables, the
  `Version` (so compaction cannot delete its files), and the table cache.
  Later writes, deletes, and compactions do not change what it shows. It
  stays usable after the snapshot it came from is released, and even after
  the database is closed and destroyed, as long as the user's `Env` lives.
- **Ownership changed to allow that.** Every open table used to point at a
  comparator that was a member of `DBImpl`, so an iterator outliving the
  database would have held dangling pointers. The internal comparator is
  stateless, so it is now one process-wide instance. The table cache is
  shared and owns the block cache. `TableCache::NewIterator` returns an
  iterator that keeps its table open, replacing compaction's private copy.
- **Errors stop a scan; they are never skipped.** A malformed internal key
  ends the scan with `Corruption`. A table that cannot be opened or read ends
  it with that error. Skipping either would hand the user an incomplete
  range with nothing to say so.
- **The new `ReadOptions` fields have explicit defaults.** With `-Wextra`,
  GCC warns when a designated initialiser omits a member that has none, so
  user code like `ReadOptions{.snapshot = s}` would otherwise have started
  warning.
- **Outliving the database is bounded.** Level-0 tables are opened when the
  iterator is created. Deeper tables open only when the scan reaches them.
  If the database is reopened and compacted meanwhile, such a table can be
  gone, and the scan then stops with an error. The API comment says so.

## Gate: scans match the model under concurrent writes

Four writers each update their own key range. In the same atomic batch, each
also writes a counter recording how many batches it has applied. Every tenth
batch is synced, so its group inserts into the memtable with the mutex
released and scans race genuinely in-flight entries.

A scanner checks each full scan as follows. For every writer, it reads the
counter, and the writer's range must equal that writer's model after exactly
that many batches. It runs at least 10 scans and continues until the writers
finish.

**Negative control.** If iterators ignore their sequence number, the gate
fails with 284 mismatched scans, and both snapshot tests fail too. A
dedicated test also pauses a synced group between its memtable insert and
its publish; neither a scan nor a `Get` may see it.

## Benchmark: scan throughput

`benchmarks/scan_bench` loads 1,000,000 keys with 100-byte values in random
order, reopens the database, and waits out compaction, which leaves tables
in levels 1-3 (5 / 49 / 2). Medians of 5 rounds on the reference machine:

| Scan | Throughput | CV |
|---|---:|---:|
| Full forward | 5.0M entries/s | 8.2% |
| Full backward | 3.2M entries/s | 3.3% |
| 100-key bounded scans | 30,423 scans/s | 3.8% |

Backward is slower because a block's entries are prefix-compressed forward.
Stepping back re-decodes from the previous restart point.

## Verification (final tree, `4907c21`)

- **Suites:** plain, ASan/UBSan, and TSan each pass **407/407**. ASan covers
  the short-key merge test.
- **Coverage:** **96.9% of lines** and 98.3% of functions. Compaction's merge
  code is at 97%, now that the unused iterator wrapper is gone.
- **Format:** the clang-format check is clean.
- **Sync negative control:** **11 of 11** sites caught.

## What went wrong

1. **A negative control that did not run, twice.** First, a `sed` pattern no
   longer matched the line after clang-format had rewrapped it. Then the
   mutant failed to compile (`-Werror`, unused variable), and the old binary
   ran. Both looked like "the gate still passes". The control now counts its
   replacements and checks that the build succeeded. The real run fails, as
   it should.
2. **A shell heredoc mangled a multi-line edit**, and a `\n` became a real
   newline inside a string literal. Edits now go through script files.

## Review

An independent review found nothing above MEDIUM. Fixed:

- **MEDIUM: out-of-bounds read on a malformed key.** The internal-key
  comparator read a trailer at `size - 8` without checking the size. The
  merging iterator compares keys before `DBIter` parses them, so a short key
  from a damaged table could be read out of bounds. Such a key is now
  ordered by its bytes and never read outside. `DBIter` reports `Corruption`
  when it reaches the key, and its backward walk now parses every key it
  steps over. A test merges a short key with a healthy child in both
  directions under ASan.
- **MEDIUM: the gate never raced a published-but-unseen group.** Unsynced
  groups insert under the mutex, so nothing was ever in flight. This was
  fixed with the hook test and synced batches described above.
- **MEDIUM: the claim about outliving `Close` was too broad.** It is now
  bounded and documented, as above.
- **LOW items:**
  - The level iterator borrows the pinned `Version`'s file list instead of
    copying it per scan.
  - Repositioning clears a stale status.
  - Scans stop at a bound instead of skipping on through hidden entries
    beyond it.
  - The gate requires a minimum number of scans rather than trusting the
    scheduler.
- **Accepted:** each live iterator keeps the tables it opened open, beyond
  `max_open_tables`. This is documented.

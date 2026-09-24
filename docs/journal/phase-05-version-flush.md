# Phase 5: Versions, MANIFEST and memtable flush

**Branch:** `feature/phase-05-version-flush`. Gate: flushed data survives restart and power loss
at every publish point; obsolete files are garbage-collected.

> **Status: in progress (paused 2026-09-24).** Engine and tests are committed; 290/290 tests pass
> in the plain build. See [Resume checklist](#resume-checklist).

## Roles and methods

| Work | Role | Method |
|---|---|---|
| MANIFEST format | Storage architect | Spec-first (`docs/format/manifest.md`) before `VersionEdit` |
| `VersionSet` commit protocol | Reliability engineer | Pre-mortem: "power fails between each two syscalls, what is lost?" |
| Flush / rotation in `DBImpl` | Senior C++ engineer | Single mutex (ADR-0004), I/O done with the lock released |
| Crash testing | Test engineer | Crash-point enumeration: crash at every mutating op of a clean run |
| Missing-file handling | Security/reliability reviewer | FMEA: each failure mode must surface, never read as "key absent" |

## Decisions

- **Phase-3 log consolidation is removed.** Recovery now flushes the replayed memtable to a
  level-0 table and commits it with the new log number in *one* MANIFEST edit. Until that
  commit the old logs stay authoritative; after it they are obsolete. One mechanism (flush)
  instead of two.
- **Commit point = MANIFEST fdatasync.** `LogAndApply` appends the edit, syncs, and only then
  makes the new `Version` current. The first edit of a session writes a fresh MANIFEST
  (full snapshot + edit), then publishes it through CURRENT: temp file, sync, rename, `SyncDir`.
  On failure the MANIFEST writer is dropped, so the next edit never appends after a torn record.
- **MANIFEST damage is fatal**, unlike the WAL's torn tail: a MANIFEST with a hole could forget a
  table and silently lose data.
- **Retiring a log syncs it first** (`NewLogFile`), so power loss can never keep a later synced
  write while losing earlier unsynced ones (prefix property).
- **`Version`s are `shared_ptr`s; the `VersionSet` tracks them via `weak_ptr`.** A reader holding
  an old Version keeps its tables alive for GC without reference-count bookkeeping.
- **GC (`DeleteObsoleteFiles`)** removes logs below the committed log number, MANIFESTs other
  than the current one, tables not referenced by any live Version nor being built
  (`pending_outputs_`), and stray `.dbtmp` files.
- **`BuildTable` verifies its output** (opens it through the `TableCache`) before the table is
  referenced anywhere, and deletes the file on any error.
- **The flush commits the immutable memtable's own last sequence** (`imm_last_sequence_`),
  not the live `last_sequence_` (see "What went wrong").
- **A missing table is `Corruption`, never `NotFound`.** The POSIX env maps `ENOENT` to
  `NotFound`; passed through, `Get` would answer "no such key" and hide data loss.

## Steps (commits)

| Commit | What | Tested by |
|---|---|---|
| manifest spec | `docs/format/manifest.md` | review |
| `579f59a` | `VersionEdit` encode/decode | `version_edit_test` |
| `0d4c4c1` | `TableCache` (unbounded; LRU in phase 6) | via flush tests |
| `e25e7a8` | `Version` / `VersionSet`, MANIFEST commit | `version_set_test` |
| `a4de998` | `BuildTable` | flush tests |
| `005b8b5` | `DBImpl`: background flush, rotation, recovery-flush, GC | `flush_test`, updated `db_test` |
| `729b697` | `FaultInjectionEnv::SetCrashAfterOps` / `mutating_ops()` | `flush_crash_test` |
| `b61210c` | fix: missing table → Corruption | `MissingTableIsReportedAsCorruption` |
| `0f231e7` | fix: flushed last sequence | `FullMemtableIsFlushedToATableAndStaysReadable`, crash test |
| `be48263` | tests: VersionSet, flush, GC, every flush crash point | — |
| `91a8dd8` | clang-format | `scripts/format.sh --check` |

## Verification (so far)

- Plain build: **290/290** tests pass (`scripts/test.sh`), format check clean.
- `flush_crash_test` crashes at every mutating filesystem op of a 150-write, multi-flush
  workload, simulates power loss, reopens, and asserts: reopen succeeds, recovered keys are
  exactly a prefix of the writes, and every acknowledged synced write is present.
- **Not yet run:** ASan+UBSan, TSan, coverage.

## What went wrong

1. **Acknowledged writes lost after a flush (data loss, found by tests).** The flush committed
   `versions_->SetLastSequence(last_sequence_)`, which included writes made to the *new*
   memtable while the flush ran. Those live only in the active log; on reopen, replay skips
   batches at or below the MANIFEST sequence as "already in a table", so they vanished.
   Found twice independently: `FullMemtableIsFlushedToATableAndStaysReadable` (key 1946 missing
   after reopen) and the crash enumeration ("key 29 present after a gap"). Fix: record
   `imm_last_sequence_` at rotation and commit that. Five whys ended at: the MANIFEST's
   sequence has the meaning "everything at or below is in a table", and the code used a counter
   with a different meaning.
2. **Missing table read as absent key.** Spotted while designing the tests (FMEA pass over
   `TableCache::Find`), test written first, then fixed.
3. Stale tests and comments still described consolidation; rewritten to describe the flush.

## Resume checklist

Branch `feature/phase-05-version-flush`, HEAD `91a8dd8`. Remaining for the phase gate:

1. Manifest fuzz harness in `tests/fuzz/` (mirror the SSTable fuzzer; run 5 min under ASan).
2. CLI test for `stratactl dump-sst` in `tests/cli/stratactl_test.sh`.
3. Consider: a crash-enumeration variant with async flush (no `WaitForFlush` per write), and a
   concurrent reader-during-flush test for TSan.
4. `docs/architecture.md` (write path, read path, flush, recovery, file lifecycle).
5. Combined independent review of phases 4 + 5 (cpp-reviewer role); fix HIGH/MEDIUM.
6. Gate: plain + ASan/UBSan + TSan (`setarch -R`) suites, coverage ≥ 90% lines
   (`gcovr ... $HOME/build/strata-cov`, delete `*.gcda` first).
7. Finish this entry, `git merge --no-ff` into `main`, tag `phase-05`.

Then phases 6–15 per `docs/roadmap.md`.

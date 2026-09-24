# Phase 3: memtable and the first working database

**Branch:** `feature/phase-03-memtable`

## Goal

> Internal keys and comparator, arena skiplist, `Open`/`Put`/`Get`/`Delete`, WAL replay;
> thread-safe (single mutex) from day one. Merge gate: `put → kill → reopen → get`; differential
> test against `std::map`; synced writes survive power loss.

After this phase strata is a real (small) database: data written by one process is there
when the next process opens it, even if the first was killed.

## Roles and methods

| Work | Role | Method |
|---|---|---|
| Internal key format | Storage architect | **Spec-first** (`docs/format/internal_key.md`) |
| `DB` API and durability default | Principal engineer | **ADR-0005** |
| Skiplist memory ordering | Senior C++ engineer | Written safety argument in the header + TSan-checked concurrency test |
| Memtable, DB | Senior C++ engineer | TDD, **differential testing** against `std::map` and a versioned model |
| Crash safety | Reliability engineer | Real `SIGKILL`, worst-case power loss, **negative controls** |
| `docs/durability.md` | Technical writer | **Diátaxis** (reference + explanation), with every claim tied to a test |
| Review | Database-internals reviewer | Independent agent, "can a write be lost, reordered, or resurrected?" |

## Design decisions

### Internal keys sort newest-first (storage architect)

`user_key ‖ fixed64(sequence << 8 | type)`, ordered by user key ascending and then trailer
**descending**. A seek for "k as of s" lands on the newest visible version in one step, and
older versions stay behind it for snapshots. The seek key uses the *highest* type so a read at
sequence s finds a write made at s. One test exists solely to pin that detail.

### A skiplist that is lock-free for readers now (senior C++ engineer)

Phase 3 takes one mutex for everything (ADR-0004), so why bother? Because the structure's
correctness under concurrent readers is a property of its *design*, and retrofitting it in
phase 9 would mean redesigning code that other layers already depend on. The safety argument
is written in the header: nodes are fully built before a release store publishes them, readers
follow links with acquire loads, the height is published relaxed (a stale height is harmless),
and nodes are never freed individually. LevelDB's `next_[1]`-indexed-past-the-end layout was
**not** copied: it is undefined behaviour in C++. Links are placement-new'd after the node instead.

### Three lookup outcomes, not two

`MemTable::Get` returns `kFound`, `kDeleted`, or `kNotPresent`. "Deleted" must stop the read path
at the memtable, or a deleted key would be resurrected from an older SSTable once flush exists.
The type system makes it impossible to forget.

### The DB API and its durability default (ADR-0005)

Move-only handle; `Get` returns `Result<std::string>`; **`sync = false` by default**, with the
consequence written in two places. Recovery **fails closed** on corruption inside the log, with an
explicit salvage switch. Oversized keys and values are **refused, never truncated**, closing the
gap the phase-2 security review found.

### Visibility after durability

`Write` appends to the WAL, then syncs if asked, *then* inserts into the memtable. A reader can
never observe a write that a crash could take away. A failed append or sync becomes a **sticky
background error**, so no later write is acknowledged behind a record recovery would stop at.

## Steps

| Commit | What | Verified by |
|---|---|---|
| `e50677f` | Internal key spec | review |
| `98eec6f` | `Random` (SplitMix64) | determinism, OneIn(4) frequency |
| `6c22096` | Internal keys, comparator, `LookupKey` | ordering, seek-key-at-own-sequence |
| `bf4fec6` | Lock-free-reader skiplist | vs `std::set`; 1 writer + 4 lock-free readers under TSan |
| `7496e3b` | `Iterator` interface + `MemTable` | versioned-model differential test |
| `e18ab05` | File naming + strict parser | 21 near-miss names rejected |
| `1fb3a85` | `Status::IsInvalidArgument` | used throughout |
| `ed171da` | `WriteBatch` size limits (poisoned batch) | 5 GiB Slices over a 1-byte buffer |
| `7090a5d` | ADR-0005 | review |
| `8f75a41` | `DB`, `DBImpl`: open, lock, replay, read, write | 19 integration tests incl. `std::map` differential |
| `6d4e2ea` | Crash tests | real `SIGKILL` ×5 rounds; power loss ×4; negative controls |
| `31c5ebf` | `docs/durability.md` | every claim cross-checked against a test name |
| `0378563` | `stratactl put/get/delete` + CLI test | separate processes recover each other's writes |
| `1741756` | UBSan fix: null `memcpy` | 5 failing tests → green |
| `ca7d17b` | TSan launcher for the CLI test | green under TSan |
| `a8b77d1` | Recovery-branch tests | `db_impl.cpp` at 100 % lines |

Every commit in the first half of the series was exported and built and tested **on its own**
(169 → 176 → 180 → 189 tests), so the history is bisectable. A small helper
(`stage_cmake.py`) staged each commit's CMake lines separately so that stayed true.

## Independent review: database-internals reviewer

A third expert role, a **database-internals reviewer** (someone who has worked on
LevelDB/RocksDB recovery), was asked one question: *can an acknowledged write be lost,
reordered, or resurrected?* It found two HIGH, four MEDIUM, and three LOW issues. All were
confirmed and fixed.

| Severity | Finding | Fix |
|---|---|---|
| HIGH | Replayed logs stay unsynced; a newer synced write + power loss yields a **non-prefix** state (a deleted key comes back) | **Consolidation**: rewrite recovered data into the new log, sync, then delete old logs |
| HIGH | `Close` racing a `Write` dereferences a null log | `closed_` guard under the mutex; honest thread-safety contract in `db.hpp` |
| MEDIUM | Salvage can reuse sequence numbers of a dropped batch that later reappears | consolidation deletes the damaged log |
| MEDIUM | `sync = false` power loss can leave zero-filled blocks, and strict `Open` then fails | damage at the end of the newest log with nothing valid after it is a torn write |
| MEDIUM | The SIGKILL test could not catch replay-order bugs (fresh keys only) | model-based workload with overwrites and deletes |
| MEDIUM | LOG rotated on every open; `error_if_exists` rotated before rejecting | append-only LOG with session banners; check first |
| LOW | Batch count overflow in `Append` | oversized mark |
| LOW | Skiplist Key alignment and destructor assumptions unchecked | `static_assert`s |
| LOW | File number `UINT64_MAX` wraps to 0 | refuse to open |

**Design decision: always consolidate, rather than only when something was dropped.** A rare
path that runs only after corruption would be the least-tested code in the engine. Running it on
every `Open` means every test exercises it, and it also removes the "logs accumulate forever"
limitation. Replay gained two rules that make it crash-safe: skip whole-duplicate batches,
reject straddling ones.

**The negative controls found a gap in the tests, not the code.** With consolidation's
`fdatasync` deleted, *no test failed*. Without it, `Open` would durably delete the old logs while
the rewritten copy was still only in the page cache, and a power loss straight after `Open` would
lose everything. `DataSurvivesPowerLossRightAfterRecovery` was written, and the control now fails
it. This is the negative-control method doing exactly its job.

## Verification

| Check | Result |
|---|---|
| All suites, plain build | 224/224 |
| ASan + UBSan | 224/224 (after the fix below) |
| TSan | 224/224 (after the launcher fix) |
| Coverage | **97.5 %** lines overall after the review fixes (97.8 % before); `db_impl.cpp`, `db.cpp`, `memtable.cpp`, `dbformat.cpp`, `filename.cpp`, `write_batch.cpp`, log reader/writer at **100 %** |
| Merge gate: kill | `DBCrash.SigkillNeverLosesAnAcknowledgedWrite`: real `SIGKILL` at random points, 5 rounds on one DB |
| Merge gate: differential | `DBTest.AgreesWithStdMapAcrossReopens`: 4,000 random operations with reopens |
| Merge gate: power loss | `DBPowerLoss.*`, four scenarios |

### Negative controls

| Deleted | Failed |
|---|---|
| `SyncDir` after creating a log | 3 power-loss tests |
| `fdatasync` on `sync = true` | 3 power-loss tests |
| the log writer's `Flush` | the `SIGKILL` test |
| consolidation (entirely) | the resurrection test |
| consolidation's `SyncDir` | the resurrection test |
| consolidation's `fdatasync` | `DataSurvivesPowerLossRightAfterRecovery` (added after this control first passed) |
| `Close`'s `fdatasync` | `CleanCloseMakesUnsyncedWritesDurable` |

Every durability call in the engine is guarded by a test that notices its absence: the v1.0
negative-control gate, applied in phase 3 rather than phase 8.

## What went wrong

- **UBSan: `memcpy` from a null pointer.** Every `Delete` passed a default-constructed `Slice`
  (null `data()`) as its value. *Why wasn't it caught earlier?* The memtable's unit tests used `""`
  literals, which have real pointers. *Why did only the full stack catch it?* Only `DB::Delete`
  builds a genuinely null Slice. **Lesson:** unit tests of a component should use the same
  argument shapes its real caller does. A regression test now passes null Slices directly.
- **A test indexed past the end of a vector.** `logs[logs.size() - 2]` assumed a second,
  empty log existed after `Close`, but a new log is created on the *next* `Open`. It surfaced
  as `basic_string::_M_create`. The tests now assert the exact number of logs first.
- **The CLI test failed only under TSan.** It ran its script outside the `setarch -R` launcher
  every other TSan test uses.
- **Escaping, again.** Two scripted edits turned `\x03` / `\n` / `\0` escapes into raw
  bytes. Byte-level edits via a helper script fixed it. Future escape-sensitive edits go
  through the editor, not through nested shell and Python quoting.

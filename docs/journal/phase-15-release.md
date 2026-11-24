# Phase 15: Release

**Branch:** `feature/phase-15-release`. **Released as:** `v1.0.0-rc.1` (ADR-0010).

## Goal

From the roadmap: ADRs, limitations, demo script, soak test, `v1.0.0`. Merge gate: **all
release gates met** (`docs/roadmap.md`).

## Roles and methods

- **Release manager:** which gates does the evidence meet, which not, and what does the tag
  claim? The answer is ADR-0010: everything but two, both reported under what ran.
- **Reliability engineer:** a soak that only runs a workload proves little. What would a leak,
  a lost write, or a slow descriptor leak look like in its output, and does the tool fail
  when it sees one?
- **Security reviewer:** how long and how well were the decoders fuzzed? The honest answer
  changed in phase 14, when the fuzzers turned out to have been built without coverage of
  the library.
- **Technical writer:** limitations, ADRs, and a demo a newcomer can run in five minutes.

## Steps

1. `feat(stratasoak)`: writer stripes with models, verification on read and after every
   reopen, scheduled reopens, resource sampling, growth checks, a JSON report.
2. `feat(fuzz)`: `--write_seeds` and `scripts/fuzz.sh`, so libFuzzer runs start from
   well-formed inputs and run every target in parallel for a set time.
3. `test`: the manifest's level check and an output-less compaction.
4. `docs`: ADR-0006 to ADR-0009 and `docs/limitations.md`.
5. `feat(scripts)`: `scripts/demo.sh`, run by ctest.
6. `chore(release)`: version `1.0.0-rc.1`.
7. `test(stratasoak)`: the growth verdict tested on samples (below).
8. `docs`: ADR-0010, this entry, the README.
9. `fix(compaction)`: keep the outputs when the MANIFEST commit may have landed (audit).
10. `fix(wal)`: damage followed by valid records in the last block fails Open (audit).
11. `docs`: the soak, the audit, and the gates rerun on the fixes.

## Release gates

| Gate (roadmap) | Result |
|---|---|
| Zero ASan, UBSan, TSan findings | **Met.** 559/559 under GCC 13 ASan+UBSan and TSan, and clang 18 ASan+UBSan (with libFuzzer targets) and TSan, on `49a5d09` (the audit fixes; 555/555 before them on `5dfb1c1`) |
| Line coverage ≥ 80% overall | **Met.** 96.8% (4795 of 4956 lines in `src/` and `include/`) |
| 100% on the log reader/writer | **Met.** `log_reader.cpp` 119/119, `log_writer.cpp` 52/52 |
| 100% on the manifest publish path | **Met.** `version_set.cpp` 218/218 (the invalid-level check is now tested) |
| 100% on the compaction merge logic | **Met but for one guard.** `compaction.cpp` 106/108: line 139 rejects a malformed internal key inside a table whose checksum is valid, reachable only with a crafted table in a live version; line 94 is the multi-line `TableOptions` initializer that every compaction with output executes, which gcov attributes as unexecuted (the output-writing tests assert those tables exist) |
| Every crash-matrix point recovers consistently with the model | **Met.** The crash suite (crash matrix, model-based restarts, disk-full and EIO injection) passes in every configuration above |
| Negative control: removing any `fsync`/`SyncDir` fails a crash test | **Met.** 11 of 11 sites caught |
| 24-hour soak: no leak, no unbounded memtable, no descriptor growth | **Reported as one hour** (ADR-0010): see Soak below |
| Fuzzers clean for one hour each (log, SSTable, manifest readers) | **Met, and the decompressors too.** See Fuzzing below |
| Every README number traceable to a run on stated hardware | **Met.** Each links to the journal that recorded it |
| Benchmark suite at CV ≤ 5% on a quiet host | **Not met** on the WSL2 laptop (phase 11); the reason for `-rc.1` |

## Fuzzing

`scripts/fuzz.sh` with clang 18 libFuzzer, ASan, and UBSan, the library and both compressors
instrumented for coverage (phase 14), seeded from each harness's well-formed inputs, 1.5 GB
RSS limit. The first run was stopped at about 22 minutes by Windows memory pressure (Claude
Code reaps background jobs when memory is low); the rest ran as four foreground chunks of
about 9.5 minutes, each resuming the saved corpus. Totals:

| Target | Seconds | Executions | Final corpus | Crashes |
|---|---|---|---|---|
| `log_reader_fuzz` | 3,601 | ≈25.8 M | 247 | 0 |
| `manifest_fuzz` | 3,616 | ≈6.4 M | 271 | 0 |
| `sstable_fuzz` | 3,605 | ≈25.2 M | 169 | 0 |
| `block_decompress_fuzz` | 3,623 | ≈51.7 M | 1,760 | 0 |

Coverage plateaued early in every target (`sstable_fuzz` at about 3,035 points,
`block_decompress_fuzz` about 3,090), which is what a well-explored decoder looks like.

## Soak

`stratasoak --duration_s=3600 --reopen_every_s=600 --sample_s=30 --check_growth=1`, release
build of `c3fa69c`, on ext4 inside WSL2 (8 vCPU, 11 GiB), started 2026-11-24 04:54 UTC from a
`wsl.exe` no Claude Code shell owned, so the memory-pressure reaper that stopped the earlier
attempts could not reach it. **Passed, exit 0.**

| | |
|---|---|
| Ran | 3,600 s, two writer stripes, two readers, scans and snapshots throughout |
| Operations | 44.1 M writes, 4.9 M deletes, 565 M reads, 11.8 M scans, 11.8 M snapshots |
| Verification | 5 scheduled reopens, each checking every key the writers had touched, plus a final check: 1.97 M verifications, no error |
| Memory (RSS) | 53–57 MiB from the second quarter on; 55.1 MiB late vs 57.3 MiB early (bound 79.0) |
| Open descriptors | 23–29 from the second quarter on; 28 late vs 29 early (bound 45) |
| Memtables | at most 4.0 MiB (bound 8.0, twice `write_buffer_size`) |
| Tables | 14–20 live across levels 0–2 after warm-up |

Throughput was about half that of the earlier, interrupted attempts at the same point
(3.5 M vs 7.7 M writes at 300 s): the laptop was busier. The soak checks errors and growth,
not speed. Two earlier attempts, stopped by memory pressure at 1,300 and about 720 seconds,
ran clean while they ran.

The two fixes below came after the soak and touch only error paths it never exercises (a
failed MANIFEST sync, a damaged log); every other gate was rerun on them.

## Pre-release audit

Before tagging, five independent read-only reviews, each given one failure class rather than
a slice of files, read the engine while the soak ran: durability and recovery (a storage
engineer's crash-window lens), concurrency and lifetimes, decoders against crafted bytes (a
security reviewer's lens, aimed at what coverage-guided fuzzing misses: inputs that need a
valid checksum), read and compaction semantics, and the HTTP stats server. Read and
compaction semantics came back clean on all six questions asked (tombstone resurrection,
snapshot visibility, level-0 order, iterator bounds, Bloom filter keys, level overlap).

**Fixed before the tag**, each with a test that failed first:

- **CRITICAL: a compaction whose MANIFEST sync failed deleted its outputs**, although the edit
  had already been flushed to the OS. Reopening replayed it, found the outputs missing, and
  garbage collection then removed the inputs: the key range was lost, with no power loss
  needed. The phase 5 fix for CURRENT had the same shape. Outputs now stay until the next open
  decides.
- **HIGH: damage inside the newest log's last block counted as a torn tail** when the records
  after it were only skipped, never applied, as always happens in a log under 32 KiB. Synced
  writes after one flipped byte were dropped silently, against ADR-0005. The reader now checks
  what it skips for a record whose checksum verifies; so do the bad-length and zeroed-header
  paths, which had ended the log silently.

**Deferred to `v1.0.0-rc.2`**, none reachable without a fault or a hostile file:

| Severity | Finding |
|---|---|
| HIGH | An exception (`bad_alloc`) in the group-commit leader leaves its stack entry in the writer queue: every later writer and `Close()` wait forever |
| HIGH | Crafted, checksum-valid tables can make reads quadratic (one restart point over a huge block; index fan-out) |
| HIGH | MANIFEST replay copies the whole version per edit: a very long MANIFEST makes Open slow; the MANIFEST is never rolled while open |
| HIGH | A forged WAL batch with a sequence past 2^56 opens once, then the MANIFEST it wrote fails every later open |
| MEDIUM | The background thread holds the DB mutex across the MANIFEST fsync and file deletion, stalling reads for as long |
| MEDIUM | After a crash during log rotation, damage at the end of a log that is no longer the newest fails Open |
| MEDIUM | A missing CURRENT beside existing tables is treated as a new database, and the old tables are collected |
| MEDIUM | Out-of-order keys in a checksum-valid table stop all writes with the wrong error class |
| LOW | Snapshot handles are checked by address, so a released one can match a newer snapshot |
| LOW | An empty `sync = true` batch does not sync earlier writes |
| LOW | The stats server: idle connections delay others by 2 s each, not 2 s in total; no Host check when bound off loopback; error replies can be reset by unread request bytes |

The audit also found the `sstable_fuzz` harness cannot get past block checksums and uses the
bytewise comparator, so block parsing on hostile content and the Bloom-filter path are
effectively unfuzzed: the next fuzzing work is a checksum-recomputing mutator.

## What went wrong

- **The first growth check was tested in the wrong place.** A 9-second soak under ASan
  "grew" from 136 to 184 MiB: ASan's quarantine holds freed memory back to catch
  use-after-free. Seconds are no warm-up and ASan is no normal build. The verdict is now
  unit-tested on made-up samples (a run that levels off passes; memory, descriptor, and
  memtable growth, and too few samples, fail), and only the long release soak asks for it.
- **A wait loop piped into `sort`** ran in a subshell, could not wait on the fuzzers it
  started, and exited, killing them: the first chunk did not count and was rerun.
- **Long background runs were stopped by memory pressure four times this phase and the
  last.** The machine also runs unrelated services in WSL (a logging server and a Node
  service). Fuzzing was split into foreground chunks; the soak, which must be one process,
  needs the reaper off.

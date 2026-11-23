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

## Release gates

| Gate (roadmap) | Result |
|---|---|
| Zero ASan, UBSan, TSan findings | **Met.** 555/555 under GCC 13 ASan+UBSan and TSan, and clang 18 ASan+UBSan (with libFuzzer targets) and TSan, on `5dfb1c1` |
| Line coverage ≥ 80% overall | **Met.** 96.7% (4777 of 4938 lines in `src/` and `include/`) |
| 100% on the log reader/writer | **Met.** `log_reader.cpp` 101/101, `log_writer.cpp` 52/52 |
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

<!-- filled in after the one-hour run -->

An earlier attempt, also stopped by memory pressure at 1,300 seconds, ran clean while it ran:
12.9 million writes and 1.4 million deletes from two writers, two scheduled reopens with
every key the writers had touched checked, 424,000 verifications, no error.

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

# Phase 14: Compression

**Branch:** `feature/phase-14-compression`.

## Goal

From the roadmap: a pluggable compressor, zstd and snappy, pinned via `FetchContent`.
Merge gate: **before/after footprint, throughput, and p99.**

## Roles and methods

- **Storage architect:** the format had reserved a compression byte in every block trailer
  since phase 4. What must it mean, exactly, so that a reader never needs a setting and a
  database can change settings without stranding a table?
- **Security reviewer (STRIDE, narrowed to Denial of service and Tampering):** both formats
  carry the uncompressed length in a header a damaged or crafted file can make up, and the
  checksum does not stop someone who computes it. What is the most a few stored bytes can
  make the reader allocate, and what does a decompressor do with hostile input?
- **Supply-chain reviewer:** the first third-party code in the engine's runtime. An
  independent dependency audit (licence, pinned commit, advisories, maintenance, build
  integration, safe API use) came before any code.
- **Performance engineer:** what does each algorithm cost and save, measured the same way
  three times, on data that can actually be compressed?
- **Test engineer:** which tests would pass with the code wrong? The review found several,
  and one had never written a table at all.

## Dependency audit

An independent audit approved both, with conditions this phase follows:

| | zstd | snappy |
|---|---|---|
| Version | v1.5.7 (2025-02-19), commit `f8745da6` | 1.3.1 (2026-09-18), commit `9c28114a` |
| Licence | BSD-3 or GPLv2: taken under BSD-3 | BSD-3 |
| Advisories at this version | none found (OSV, NVD) | none found; a 2026 decoder fix is inside 1.3.1 |
| Built as | static, no programs, tests, contrib, threads, or legacy (v0.1–v0.7) decoders | static, no tests, benchmarks, submodules |
| Conditions | never allocate from the header unchecked; decompress into a caller-bounded buffer; check the length | use `RawUncompress` into a checked buffer, never the string-resizing `Uncompress`; fuzz the decoder |

## Design decisions

| Decision | Alternatives | Why |
|---|---|---|
| Each block names its encoding in its trailer; the reader needs no setting | A table-level or database-level setting | A database can change settings at any time and keep reading every table it ever wrote |
| A `Compressor` interface, one subclass per algorithm, selected by the trailer byte | A `switch` in the reader and writer | Adding an algorithm is a type value, a subclass, and one case: the roadmap's "pluggable" |
| Store compressed only when that saves an eighth | Always store compressed | Incompressible data then costs a compression attempt at write time and nothing on every later read (LevelDB's rule) |
| The filter block is never compressed | Compress it too | It is read whole into memory on open and is high-entropy by construction |
| Compressed blocks may expand to at most 4 MiB; a larger block is stored raw | No bound; 64 MiB | A crafted block of a few bytes with a valid checksum could otherwise make every read allocate and zero whatever its header claims. 4 MiB is far above any normal block, and only a value of megabytes reaches it (and is then simply stored raw) |
| A claimed length of 0 is corruption | Accept it | The writer never compresses an empty block, and a zstd skippable frame has content size 0 |
| One zstd context per thread | `ZSTD_compress` per call | The one-shot calls create and free a context for every block |
| Default `Options::compression = kNone` | Default to snappy, as LevelDB does | On this host snappy costs 14–18% of throughput for 34% less disk. That is a good trade for many deployments and a surprise for others; enabling it is one line. Recorded in `docs/limitations.md` (phase 15) |
| `FetchContent` pinned to commit hashes | Release tarballs; system packages | The roadmap asks for `FetchContent`; a hash cannot be moved, a tag can. After the first fetch nothing is contacted again |
| Sanitizer flags copied onto both libraries; zstd's assembly off in those builds | Leave them uninstrumented | Fuzzing a decompressor that ASan cannot see into misses the bugs fuzzing is for |

## Steps

1. `build(deps)`: zstd and snappy fetched, pinned, and built minimal.
2. `feat(table)`: the compressor interface, both algorithms, the trailer types, the
   bounds, `Options::compression` and `compression_level`, and the tests and fuzz target.
3. `feat(stratabench)`: `--compression`, `--compression_level`, `--compression_ratio`, and
   the loaded database's footprint in the report.
4. `docs`: the format of compressed blocks, and the README's build steps.
5. `fix(table)`: the review's findings (below).

## Measurement

`stratabench`, Release build of `8df7002`, 1 million keys, 100-byte values compressible to
half (`--compression_ratio=0.5`, as LevelDB's `db_bench` uses), a warm 8 MiB block cache;
three passes with the order rotated, each workload 5 rounds of at least 3 seconds; p99 from
the open-loop harness at 40,000 operations per second (4 workers, 10 s per round). Same
i5-12450H laptop under WSL2 as every earlier phase.

| | Footprint (SSTables) | fillrandom ops/s | readrandom ops/s | p99 at 40k ops/s |
|---|---|---|---|---|
| none | 107.9 MiB | 440k (443, 440, 437) | 219k (249, 219, 214) | 125 µs (110, 125, 132) |
| snappy | **71.1 MiB (−34%)** | 380k, −14% (409, 380, 379) | 180k, −18% (187, 177, 180) | 120 µs (105, 120, 124) |
| zstd (level 3) | **61.4 MiB (−43%)** | 247k, −44% (263, 245, 247) | 105k, −52% (110, 103, 105) | 128 µs (106, 128, 130) |

Medians of the three passes, with each pass in parentheses. Footprints are identical in
every pass (the load is deterministic). The worst within-run CV was 17% (one `none` fill
pass) and 9% (a snappy read pass), so differences of a few percent are noise here, but
the gaps above are far larger than the spread between passes.

Reading the table: on random reads that mostly miss an 8 MiB cache, every lookup
decompresses a block, which is where zstd's read cost comes from. Snappy keeps most of the
throughput for a third less disk. At a fixed, sustainable rate the p99 is the same for all
three within the spread: compression costs throughput capacity, not latency at a moderate
load. On values that do not compress (`--compression_ratio=1`), the one-eighth rule stores
the blocks raw, so the cost is the compression attempt on write only.

## Verification (final tree, `8df7002`)

- **Suites:** plain, ASan/UBSan, and TSan each pass **546/546** with GCC 13, and so do
  ASan/UBSan with the libFuzzer targets (now built with `-fsanitize=fuzzer-no-link`
  throughout: 221 compile commands carry it) and TSan with clang 18. That is phase 13's 530
  plus the compression unit, engine, and fuzz smoke tests.
- **Coverage:** **96.7% of lines** (4775 of 4938). `compression.cpp` is at 90%: what is
  left is the two out-of-memory guards for zstd contexts, a zstd compression error the API
  cannot produce on valid input, and the "this build leaves the algorithm out" branch,
  which only exists in a build without it.
- **Format:** clean.
- **Sync negative control:** **11 of 11** sites caught. Compression adds no sync calls.

## Review

An independent review found **no CRITICAL or HIGH** issues. It traced every block read to
`ReadBlock` (data, index, metaindex, filter), confirmed the checksum is verified before
anything decompresses, that both bounds come before any allocation, that `written ==
length` rejects short output and extra frames, and that old tables read as before.
Findings:

- **MEDIUM, fixed: libFuzzer had no coverage feedback outside the harness file.** Neither
  the library under test nor the compressors were built with coverage instrumentation, so
  every fuzz target in CI since phase 2 mutated nearly blind. `STRATA_FUZZ_LIBFUZZER` now
  adds `-fsanitize=fuzzer-no-link` to everything instrumented.
- **MEDIUM, fixed: 64 MiB per crafted read.** The bound is now 4 MiB, and a claim of 0 is
  corruption.
- **MEDIUM, fixed: a zstd context per block.** Now one per thread.
- **MEDIUM, fixed: tests that would pass with the code wrong.** The one-eighth rule is
  tested at its boundary, the size limit exactly at and one past it, and the mixed-setting
  test writes disjoint keys so every round must be read. The zstd-levels test, made
  strict, failed at once: 0 bytes against 0 bytes. It had never written a table, because
  3,000 records fit in the default 4 MiB write buffer and stayed in the log.
- **MEDIUM, documented: a table in an algorithm this build leaves out.** Reading it is
  `NotSupported`; a compaction that needs it fails, and the database stops accepting
  writes with that error, as it does for any other unreadable input. Building with every
  algorithm a database has used avoids it.
- **LOW, fixed:** snappy's submodules are no longer cloned, nothing re-fetches after the
  first configure, and `--compression_ratio` rounds instead of truncating.
- **LOW, left:** `db_impl.cpp` is over the 300-line limit; it was before this phase.

## What went wrong

- **A test passed for the wrong reason for its whole life.** The zstd-levels test compared
  two empty directories, `<=` held, and nothing noticed. Making the assertion strict is
  what exposed it, which is the review's point about tests that pass with the code wrong.
- **The fuzzers were weaker than every journal since phase 2 implied.** Coverage-guided
  fuzzing needs the code under test instrumented, not only the harness; nobody had checked
  that the library objects carried the instrumentation. Found by a reviewer asking whether
  the decompressors were.
- **Two long runs were stopped by Windows memory pressure** (the benchmark, then a gate
  run). The work was redone in foreground steps of under ten minutes each.
- **CMake refused `target_link_libraries(libzstd_static PRIVATE strata_sanitizers)`**:
  zstd exports its targets, and an exported target cannot depend on an unexported one. The
  sanitizer flags are copied as compile options instead.

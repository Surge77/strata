# Phase 1: primitives

**Branch:** `feature/phase-01-primitives`

## Goal

> `Slice`, `Status`, `Result<T>`, varint coding, crc32c, `Arena`, `Env`/`PosixEnv`, `LOCK`
> file, logging, `Histogram`. Merge gate: crc32c matches the RFC 3720 vectors.

These are the vocabulary types every later phase is written in. A mistake here spreads
everywhere, so the bar is "boring and fully tested", not "clever".

## Roles and methods

| Work | Role | Method |
|---|---|---|
| Review the whole roadmap before continuing | Principal engineer | Pre-mortem → ADR-0004 |
| `Env` interface shape | Storage architect | ADR-style trade-off (below) |
| Every component | Senior C++ engineer | TDD |
| `LOCK` file and `SyncDir` | Reliability engineer | FMEA table (below) |
| `Histogram` | Performance engineer | Checked against exact percentiles on known data |

## State at the start of this session

The first five commits of the phase already existed: `Slice` (ADR-0002), `Status`/`Result<T>`
(ADR-0003), fixed-width and varint coding, CRC-32C with a hardware path and a portable
fallback, and the `Arena`. Baseline: 63 tests passing (`scripts/test.sh`).

## Design decisions

### `Env`: one choke point for every system call (storage architect)

| Option | Verdict |
|---|---|
| Call POSIX directly from each component | Rejected: durability could never be tested without real power loss |
| `std::filesystem` + `std::fstream` | Rejected: no `fdatasync`, no directory fsync, no `pread`, exceptions on error |
| Abstract `Env` with a POSIX implementation (LevelDB's shape) | **Chosen**: test doubles can model power loss, full disks, failing fsync |

- `WritableFile` separates **Append / Flush / Sync**, and its header states what survives
  what. `SyncDir` is a separate `Env` call, because fsync on a file does not make its
  directory entry durable.
- Reads return `Result<Slice>` into caller scratch memory: no allocation per read, and the
  error model of ADR-0003 is kept.
- `LockFile` returns an RAII `FileLock`; `PosixEnv` also keeps a registry of paths locked by
  this process, because `fcntl` locks never conflict within one process.
- `Env::Default()` is intentionally leaked so background threads can still use it during exit.

### FMEA for the `LOCK` file and `SyncDir` (reliability engineer)

| Operation | Failure mode | Effect | Detection / mitigation | Test |
|---|---|---|---|---|
| `LockFile` | second `Open` in the same process | two writers corrupt the DB | in-process registry returns `Busy` | `LockIsExclusiveWithinOneProcess` |
| `LockFile` | another process holds it | two writers | `fcntl F_SETLK` returns `EAGAIN` → `Busy` | `LockExcludesOtherProcessesUntilReleased` (fork) |
| `LockFile` | `open`/`fcntl` fails midway | registry entry leaks, lock is permanently "held" | registry entry removed on every error path | code review |
| `Sync` | `fdatasync` fails (`EIO`) | data believed durable is not | error returned, never swallowed | phase 8 EIO injection |
| `SyncDir` | directory missing | new file's name not durable | `NotFound` returned | `SyncDirSucceedsOnDirectoriesOnly` |
| `close` | returns `EINTR` | retry would close someone else's fd | never retry `close` | code review |

### Logger (senior C++ engineer)

- Public interface so embedders can redirect engine logs.
- `Log` returns `void`: a full diagnostics disk must not become a failed `Put`.
- Format outside the lock, then `Append` + `Flush` inside it: lines never interleave.
- Flushed, not fsynced: survives a process crash, never costs a disk barrier.

### Histogram (performance engineer)

Compared a fixed LevelDB-style bucket table, storing all samples, and log-linear buckets.
Chose **log-linear, 16 sub-buckets per power of two**: 976 counters (8 KiB), stated error
bound of 1/16 at any magnitude, and `Merge` by addition so threads record without contention.

## Steps

| Commit | What | Verified by |
|---|---|---|
| `9068662` | `Slice` as `std::string_view` | slice_test |
| `3c3050f` | `Status` and `Result<T>` | status_test, result_test |
| `c0e9887` | fixed-width and varint coding | coding_test |
| `4a746c9` | CRC-32C (hardware and portable) | crc32c_test, including the RFC 3720 vectors |
| `8555547` | `Arena` bump allocator | arena_test, ASan |
| `162abdf` | Roadmap revision (ADR-0004) and this journal | review |
| `0e78a86` | `scripts/format.sh` (apply or check clang-format) | ran it on all sources |
| `edce85a` | `Env`, `EnvWrapper`, `PosixEnv`, `LOCK` via `LockFile` | env_posix_test (23 tests), incl. cross-process lock via `fork` |
| `5116078` | `Logger` + `NewFileLogger` | logger_test: regex line shape, 8 threads × 500 lines intact |
| `d6137b0` | `Histogram` | histogram_test: vs exact percentiles of 200k log-normal samples |
| `d6d9a90` | Review fixes: sticky write errors, inode-keyed lock registry, `readdir` errors | /dev/full tests, lock-alias test + negative control |
| `3eb6482` | Review fix: Welford mean/deviation with Chan merge | 1e12 ± 1 deviation test, merged-deviation test |

## Independent review

After the gate passed, a separate **C++ reviewer** agent read the new code cold, knowing only the
project's conventions (ADR-0003) and the verification results. Each finding was confirmed
against the code before anything was changed (receiving-code-review discipline: verify, then
fix, and never accept a finding just because a reviewer said it):

| Severity | Finding | Verdict | Resolution |
|---|---|---|---|
| HIGH | `GetChildren` treats a `readdir` failure as end of directory | Confirmed | clear/check `errno` |
| MEDIUM | Lock registry keyed on path spelling, not file identity | Confirmed | key on `(st_dev, st_ino)` |
| MEDIUM | Failed flush discards the buffer, and later calls carry on | Confirmed | sticky errors, documented in the `WritableFile` contract |
| LOW | `write()` returning 0 would spin forever | Confirmed (theoretical) | treat as error |
| LOW | Deviation is called "exact" but uses a cancellation-prone formula | Confirmed | Welford + Chan merge |

Fixing the lock finding exposed a trap the reviewer had not mentioned: closing *any*
descriptor for a file releases *all* of a process's `fcntl` locks on it, so an "open, see it's
held, close" check unlocks the real holder. A **negative control** proved the new test catches
it: with that pattern reintroduced, `LockIsKeyedOnTheFileNotThePathSpelling` fails with "a
rejected LockFile released the real lock".

## Verification

Run inside WSL (Ubuntu 24.04, GCC 13.3) with the `~/.local/strata-tools` CMake/Ninja:

| Check | Command | Result |
|---|---|---|
| Unit tests | `scripts/build.sh && scripts/test.sh` | 111/111 passed (final, after review fixes) |
| AddressSanitizer + UBSan | `scripts/build.sh --asan` + `scripts/test.sh` | 111/111, no reports |
| ThreadSanitizer | `scripts/build.sh --tsan` + `scripts/test.sh` | 111/111, no reports |
| Coverage | `scripts/build.sh --coverage` + `gcovr --filter src/ --filter include/` | **94.9 % lines**, 99.1 % functions (gate: 80 %; measured before the review fixes) |
| Bisectability | phase-2 work stashed, branch built and tested alone | clean |
| Formatting | `scripts/format.sh --check` | clean |
| Merge gate | crc32c RFC 3720 vectors | pass (`crc32c_test`) |

Not run locally (no Clang on this machine): clang-tidy, the Clang CI builds, and libFuzzer.
These run in CI.

## What went wrong

- **Formatting failures would only have surfaced in CI.** The first `Env` commit had one-line
  functions clang-format reflows. *Why?* There was no local way to run the CI check.
  *Why?* Phase 0 wired clang-format into CI only. **Fix:** `scripts/format.sh`, run before
  every commit from now on.
- **`-Wuseless-cast` broke the Histogram build.** `static_cast<std::size_t>` of a `uint64_t`
  is a no-op on 64-bit Linux, and GCC treats it as an error under `-Werror`. Removed the casts
  and added a comment, so nobody re-adds them "for portability".

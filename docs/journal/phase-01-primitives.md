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

(Filled in as the phase proceeds.)

## Steps

| Commit | What | Verified by |
|---|---|---|
| `9068662` | `Slice` as `std::string_view` | slice_test |
| `3c3050f` | `Status` and `Result<T>` | status_test, result_test |
| `c0e9887` | fixed-width and varint coding | coding_test |
| `4a746c9` | CRC-32C (hardware and portable) | crc32c_test, including the RFC 3720 vectors |
| `8555547` | `Arena` bump allocator | arena_test, ASan |

## Verification

(Filled in at the end of the phase.)

## What went wrong

(Filled in as it happens.)

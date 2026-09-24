# Phase 2: log

**Branch:** `feature/phase-02-log`

## Goal

> `FaultInjectionEnv` (power-loss model), block-framed log writer/reader, `WriteBatch`
> encoding, corruption resynchronization, log-reader fuzz harness. Merge gate: every
> truncation offset handled; unsynced tail lost on simulated power loss, synced prefix intact.

The log is the durability boundary: a `Put` is acknowledged once its record is in the log.
Everything in this phase exists to make one sentence true: *a synced record always comes back,
and an unsynced one disappears cleanly, never as corruption.*

## Roles and methods

| Work | Role | Method |
|---|---|---|
| Log and batch byte formats | Storage architect | **Spec-first**: `docs/format/log.md` and `write_batch.md` committed before any code |
| `FaultInjectionEnv` | Reliability engineer | Worst-case POSIX model (below) |
| Writer, reader, `WriteBatch` | Senior C++ engineer | TDD |
| Reader and batch decoder | Security reviewer | STRIDE (tampering, DoS): every length checked against the bytes present; fuzzing |
| Test suite | Test engineer | "What would make this pass while the code is wrong?" plus **negative controls** |
| Coverage gaps | Senior C++ engineer | Coverage-driven: test the branch, or delete it if unreachable |

## Design decisions

### The power-loss model: worst case, not typical case (reliability engineer)

A crash test is only as good as its model of what a crash destroys. `FaultInjectionEnv` takes
the **weakest guarantees POSIX gives**:

- file bytes after the last successful `Sync` are lost;
- directory changes since the last `SyncDir` of that directory are **undone**, newest first.
  A rename over an existing file gets the old file back.

Real hardware may keep more, never less. So code proven correct against the worst case is
correct against every possible outcome. The alternative, simulating "a random amount survives",
finds bugs only by luck and can't prove their absence.

### Checksum mismatch drops the rest of the block (storage architect, security reviewer)

The first draft of the spec said "skip the fragment". While designing the reader, that was
changed: after a checksum failure the *length field itself* is unverified, and following it
could land mid-payload and read arbitrary bytes as a header. Dropping to the next 32 KiB
boundary costs at most one block and can't be steered by corrupt data. The spec was updated
in the same commit series.

### The writer poisons itself (senior C++ engineer)

After a failed append the file may hold a partial fragment, so the writer's block offset no
longer matches reality. Any further record would be misframed. The writer returns the first
error forever after, matching phase 1's sticky-error `WritableFile` contract. The owner (the
WAL in phase 3) must start a new log file.

### Portable fuzzing (security reviewer): ADR-0004 in practice

The harness implements `LLVMFuzzerTestOneInput`, so CI can run it under libFuzzer. This
machine has only GCC, so it also links a small mutation driver. The driver has no coverage
guidance, but under ASan it still catches the crash class that matters for decoders: reads past
a buffer on a malformed length.

## Steps

| Commit | What | Verified by |
|---|---|---|
| `17dd54d` | Log and WriteBatch format specs | review against the worked example |
| `7140423` | `FaultInjectionEnv` + `strata_test_support` library | 11 tests: every undo path, both outcomes of the rename-publish pattern |
| `f29cb47` | Log writer and reader | 23 tests incl. **every truncation offset**; negative control |
| `3607d49` | Power-loss tests (new `crash` suite) | 25 randomised outages recover exactly the synced prefix |
| `492f3e7` | `WriteBatch` + internal accessors + `ValueType` | 13 tests incl. every truncation of an encoded batch |
| `6f63346` | Fuzz harness, GCC driver, CI libFuzzer job | 5-minute ASan run: 3,542,016 inputs, no findings |
| `2991715` | Atomic coverage counters | gcovr stopped failing on negative hits |
| `9e8c9e9` | Flush-failure test | writer at 100 % lines |
| `77f842e` | Remove unreachable batch check | batch at 100 % lines, invariant documented |
| `b34f60e` | Review fix: varint32 length prefixes (spec conformance + latent truncation) | 6-byte varint rejected; 1-byte encoding pinned |
| `7deb904` | Review fix: honest `FaultInjectionEnv` thread-safety contract | documentation |

## Independent review: security reviewer

This phase's reviewer took a different role from phase 1's: a **security reviewer** working from
STRIDE, narrowed to tampering and denial of service, with every byte on disk treated as
attacker-controlled. The reviewer checked the reader for unbounded growth, hangs, and overflow
in offset arithmetic (all verified safe, with reasons), and found:

| Severity | Finding | Verdict | Resolution |
|---|---|---|---|
| MEDIUM | Length-prefixed slices use varint64; the spec says varint32 | Confirmed, **and worse than reported** | varint32 everywhere |
| MEDIUM | `FaultInjectionEnv` check-then-act races on the same path | Confirmed; the engine never does this | contract narrowed in the docs |

The first finding hid a second bug. The memtable stores **32-bit** lengths, so a value of 4 GiB or
more would have been accepted by `WriteBatch` with a 64-bit length and then silently truncated
on its way into the memtable. Five whys: the formats disagreed → phase 1 chose varint64 "to be
safe" → no single place defined the maximum key or value size → **there was no size limit at
the API boundary**. The fix is varint32 in the codec, plus an `InvalidArgument` check for sizes of
4 GiB or more when `DB::Put` lands in phase 3.

## Verification

| Check | Result |
|---|---|
| All suites (unit, crash, fuzz smoke), plain build | 163/163 |
| ASan + UBSan | 163/163, no reports |
| TSan | 163/163, no reports |
| Coverage, overall | **96.4 %** lines, 99.2 % functions |
| Coverage, log reader / log writer / `write_batch.cpp` | **100 % / 100 % / 100 %** (release gate asks 100 % on the log) |
| Merge gate: every truncation offset | `Log.EveryTruncationOffsetYieldsACleanPrefix`: all cuts return a clean prefix, no reports |
| Merge gate: power loss | `LogPowerLoss.*`: synced prefix intact, unsynced tail gone, 25 random outages |
| Fuzzing | GCC driver under ASan+UBSan, **5 minutes** (not the 1-hour gate), 3.5 M inputs, clean |

### Negative controls run in this phase

| Deliberate bug | Test that must fail | Result |
|---|---|---|
| Reader reports a torn tail as corruption | `EveryTruncationOffsetYieldsACleanPrefix` | failed at "cut at 7": the test works |

## What went wrong

- **A too-slow gate test.** The truncation test first took ~10 s, because checking every cut
  is quadratic in log size. *Why?* 9 of 60 records spanned blocks. Cutting that to 3 still
  exercises every fragment type and brought it to ~3 s.
- **Hex-escape trap in a test.** `"\x00\x01d"` is two characters, not three: `\x01d` is one
  escape (0x1d). GCC caught one instance ("hex escape sequence out of range"). The fix is
  adjacent literals (`"\x01" "d"`), with a comment so nobody "simplifies" it back.
- **gcovr "negative hits".** Five whys: the report failed → counters were negative → threaded
  tests raced on non-atomic counters → GCC's `--coverage` default is non-atomic → nobody had
  run coverage over a threaded test before. Fix: `-fprofile-update=atomic`, in the build
  system so CI gets it too.
- **A GCC false positive** (`-Wnull-dereference` inside `istreambuf_iterator`) in the fuzz
  driver. Worked around by reading corpus files through strata's own `Env`, not by weakening
  the warning set.

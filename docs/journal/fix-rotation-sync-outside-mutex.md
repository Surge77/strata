# Fix: WAL rotation without the DB mutex

**Branch:** `fix/rotation-sync-outside-mutex`, between phases 11 and 12, as decided when
phase 11 found the stall (see `phase-11-benchmarks.md`).

## Goal

Phase 11's open-loop runs showed a 62 ms response p99 at half load. The cause:
`NewLogFile` created the new log, fsynced the directory, and fsynced the retired log (up
to 4 MB unsynced) while holding the DB mutex, so every `Get`, snapshot, and iterator
waited behind that fsync. That also contradicted the `DBImpl` comment saying the mutex
is never held across I/O.

## Roles and methods

- **Reliability engineer:** the fsync of the retired log must stay, and must still finish
  before the new log takes a write. Otherwise power loss could keep a later synced write
  and lose earlier acknowledged ones. Only *where the mutex is* may change.
- **Senior C++ engineer:** list every reader of the state that the leader touches while
  unlocked, and why each one is safe.
- **Test engineer:** a deterministic test that fails on the old code, with no timing luck.

## Design

The leading writer releases the mutex around the three I/O calls and takes it back to
install the new log. This is safe because:

| State | Why it is safe unlocked |
|---|---|
| `log_`, `log_file_` | only the leader touches them; other writers are queued behind it |
| `mem_` | readers keep reading it; no writer can add to it until the leader finishes |
| `imm_` | a rotation starts only when it is null, and only a rotation sets it |
| obsolete-file GC | keeps every log at or above the committed log number, and the new one is above it |
| `Close` | waits for the writer queue to drain before touching the log |

## Steps

1. `test(db)`: `rotation_stall_test` parks the rotating writer inside the retired log's
   `Sync` (an `EnvWrapper` with a latch) and requires `Get`, `GetSnapshot`, and
   `NewIterator` to finish within 2 s. On the old code, both read tests failed after 2 s.
   The first version of the test then hung, because the failed future's destructor
   waited for the blocked `Get`. It now releases the latch before asserting.
2. `fix(db)`: `NewLogFile(lock)` and `SwitchMemTable(lock)` release the mutex around
   the I/O. All three tests pass.

## Verification

clang-format, plain, ASan/UBSan, and TSan builds all green (438 tests). Line coverage was
96.9%. The fsync negative control still catches every removed `Sync`/`SyncDir`, including
both rotation syncs (`db_impl.cpp:275` and `:281`).

## Measurement: inconclusive, recorded as such

stratabench open-loop (90% gets, 10% puts, 4 workers) at the same offered rates as
phase 11, Release builds of `main` (`da7a1fa`) and the fix (`7b43945`), interleaved
A B A B:

| Run | 99,534 ops/s p99 | 199,068 ops/s p99 |
|---|---:|---:|
| before, pass 1 | 2.5 ms | 221 ms (saturated) |
| after, pass 1 | 40.1 ms (saturated) | 228 ms (saturated) |
| before, pass 2 | 1.1 ms | 55.6 ms |
| after, pass 2 | 13.6 ms | 14.5 ms |

The differences between passes of the same build are larger than any difference between
builds, so this measurement says nothing either way. The reason is in the harness, not
only the noise: a worker that issues the rotating write is blocked for the whole fsync,
and every arrival scheduled on that worker queues behind it. The fix lets the *other*
workers' reads proceed, which the deterministic test proves. It cannot shorten the
rotating write itself, because that write must wait for the retired log to be durable.

The remaining tail is therefore the rotating write's own fsync. Removing it would mean
syncing the retired log in the background and making only the next *synced* write wait
for it. That changes the durability protocol, so it is noted as a follow-up and not done
here. Per-operation-type latency in the open-loop harness would also separate the two
effects. That is a harness change for later.

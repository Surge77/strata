# Phase 8: Crash tests

**Branch:** `feature/phase-08-crash-tests`. Gate: removing any single `fsync` or
`SyncDir` call must make a crash test fail.

> **Status: merged into `main` and tagged `phase-08` (2026-10-06).**

## Starting point

Phase 2 built `FaultInjectionEnv`. It writes real files on ext4 and models the
worst case POSIX allows on power loss: unsynced appends are lost, and
directory changes since the last `SyncDir` are undone. It can also crash the
process after exactly N filesystem operations. Phases 3-5 used it for WAL,
recovery, and flush crash tests. Nothing yet crashed the engine during a
compaction, injected disk-full or read errors, or proved that the crash tests
would notice a missing sync.

## What was built

1. **Disk-full and read errors.** `SetDiskFull(suffix, bytes_left)` fails
   appends with "No space left on device" once a byte budget is spent. The
   append that crosses the limit writes what fits first, as a real `ENOSPC`
   can. `SetReadFailure(suffix)` fails every read from matching files, like
   `EIO`.
2. **Error-injection tests.** These cover disk full in the log, in a flush,
   and in a compaction, read errors on `Get` and during compaction, and a
   sync failure during compaction. In every case the fault surfaces as an
   error, never as missing data or a wrong value. Nothing already safe is
   destroyed, and a reopen after the fault clears recovers every
   acknowledged write.
3. **Crash matrix.** A workload of puts, deletes, synced writes, flushes, and
   manual and automatic compactions over 16 keys is crashed after every one
   of its 643 filesystem operations, followed by power loss. The recovered
   state must equal the reference model after some prefix of the operations:
   at least everything synced or flushed, at most one operation past the last
   acknowledged one. Each recovery then compacts levels 0-2 and must read back
   the same state. Two clean runs must issue identical operations, so every
   crash point names one moment.
4. **Model-based restarts.** 12 seeds of 240 operations, in segments that end
   in a random crash or a clean close. After each crash the model adopts the
   recovered prefix and the workload continues.
5. **The sync negative control** (`scripts/sync_negative_control.sh`). For
   each `Sync`/`SyncDir` call in `src/`, it replaces that call with
   `Status::Ok()`, rebuilds, runs the crash suite, and restores the file. A
   site passes only if some crash test fails. The script first requires the
   unmodified tree to pass, so a test that already fails or flakes cannot
   make every site look caught.
6. **Recovery and overlap matrices.** Open itself is crashed at each of its
   31 filesystem operations on a database that needs real recovery: replay,
   the recovery flush, the new MANIFEST and CURRENT, and old-log deletion.
   A sampled matrix (60 random crash points) lets background flushes and
   compactions overlap the writes.

## Gate: every sync is load-bearing

The first negative-control run caught 9 of the 11 sync sites. Two could be
removed with every crash test still passing:

- `NewLogFile`'s `SyncDir`, which makes a new log's name durable;
- the `Sync` of the log being retired at rotation.

Both protect one window: the memtable has rotated to a new log, and the
retired memtable's flush has not committed. A synced write in that window
promises that it, and every earlier write still only in the old log,
survive. No test put a synced write there deterministically. The matrix
waits for each flush, and the older flush test reaches the window only by
chance.

`rotation_power_loss_test` now does it on purpose. It pauses flushes, writes
until the memtable rotates, makes one synced write, and loses power. That
write and every earlier one must survive. Re-running the control on those two
sites reported both caught by the new test.

## Verification (final tree, `bf18e1d`)

- **Sync negative control:** after a green baseline, **11 of 11** sync sites
  are caught.

  | Site (line numbers as of `bf18e1d`) | Caught by |
  |---|---|
  | `builder.cpp:27` (table sync), `:30` (table name) | flush crash matrix |
  | `compaction.cpp:85` (outputs' names), `:124` (output sync) | flush crash matrix |
  | `db_impl.cpp:260` (new log's name), `:266` (retired log sync) | overlap crash matrix, rotation test |
  | `db_impl.cpp:363` (synced write) | crash matrix |
  | `db_impl.cpp:490` (sync at Close) | model-based restarts |
  | `version_set.cpp:231` (temporary CURRENT) | crash matrix |
  | `version_set.cpp:247` (CURRENT rename), `:276` (MANIFEST commit) | power-loss recovery test |

- **Suites:** plain, ASan/UBSan, and TSan each pass **378/378**. They ran with
  2 build jobs, one after another, because of memory limits.
- **Coverage:** **97.5% of lines** and 99.1% of functions.
- **Format:** the clang-format check is clean.
- **Runtime:** the crash matrix (643 points) takes about 52 s, the recovery
  matrix (31 points) about 3 s, the overlap matrix (60 points) about 5 s, and
  model restarts (12 seeds) about 3 s. A full negative-control run takes
  about an hour on this machine.

## What went wrong

1. **Two syncs were untested**, as described above. Writing the rotation
   test also showed why a 4 KiB write buffer cannot express that window. An
   empty memtable already exceeds it, so the synced write itself would need
   a second rotation and wait for the paused flush. The test uses 64 KiB.
2. **A real bug: a MANIFEST read error was reported as `Corruption`.** The
   log reader passes an `EIO` to its reporter. `VersionSet::Recover` wrapped
   whatever it got as `Corruption`, so a transient read error would tell the
   user to repair an undamaged database. The reader's code is now kept. A new
   error-injection test found it.
3. **My first guess was wrong.** I expected the `SyncDir` after renaming
   `CURRENT` to be the one the fault model could not catch. It was caught;
   the two misses were the rotation syncs.
4. A new test kept a closed `DBImpl` alive into the reopen. The `LOCK` is
   released only on destruction, so the reopen got `Busy`. The database is
   now scoped.

## Review

An independent review found nothing above MEDIUM. Fixed:

- **MEDIUM:** recovery itself was crashed only by chance. A dedicated
  recovery matrix now covers it.
- **MEDIUM:** no test overlapped writes with background flush or
  compaction. A sampled overlap matrix now does.
- **MEDIUM:** compactions were paused only after `Open` returned, so one
  could start in between. `DBImpl::OpenForTest` pauses before the thread
  starts.
- **MEDIUM:** the negative control had no baseline. It now requires a green
  baseline run first.
- **LOW:**
  - Read errors in the middle of a merge are now tested, not only at table
    open.
  - Disk-full now writes a partial table, not nothing.
  - The "one past acknowledged" allowance now applies only to a failing
    write.
- **Uncovered paths:**
  - Disk full on the MANIFEST and CURRENT.
  - Sync failure at the MANIFEST commit.
  - Read errors during log and MANIFEST replay; the MANIFEST case surfaced
    the bug above.

Accepted:

- **The determinism check compares operation counts, not full traces.** Every
  background step finishes before the foreground continues, so equal counts
  mean equal sequences in practice.
- **Torn or garbage tails beyond truncation are not modelled.** The phase 2
  log tests cover a corrupt tail; the power-loss model only truncates.
- **`ENOSPC` when creating a file** (as opposed to appending) cannot be
  injected.

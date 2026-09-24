# What "durable" means in strata

This page says exactly what survives what. Every guarantee listed here has a test that fails
if the guarantee breaks, and the test is named next to it. A guarantee without a test is not
listed. Current as of **phase 3**: one memtable, all data in the write-ahead log.

## The short version

| Event | `Put` with default options | `Put` with `WriteOptions{.sync = true}` |
|---|---|---|
| The process crashes (`kill -9`, segfault, OOM kill) | **survives** | **survives** |
| The machine loses power or the kernel crashes | recent writes may be lost (Linux typically writes dirty pages back within ~30 s) | **survives** |

A write that was *not* acknowledged (its `Put` had not returned when the crash happened) may or
may not survive. It will never survive *partially*, and it will never corrupt anything written
before it.

**A clean `Close()` makes every write durable**, including default `sync = false` writes: it
fdatasyncs the log before releasing the database.

## How a write becomes durable

```
Put(k, v)
  │
  ├─ encode as a WriteBatch, stamp the next sequence number
  ├─ append to the write-ahead log as ONE record ─── a record is recovered whole or not at all
  ├─ flush the record to the operating system ────── survives a process crash from here on
  ├─ if sync: fdatasync the log ──────────────────── survives power loss from here on
  └─ apply to the memtable ───────────────────────── visible to Get from here on
Put returns Ok
```

The durability boundary comes *before* visibility. A reader can never see a write that a crash
could then take away.

## The guarantees, and the tests that hold them

| Guarantee | Test |
|---|---|
| Every acknowledged write survives a real `SIGKILL`, at random moments, across repeated kills | `DBCrash.SigkillNeverLosesAnAcknowledgedWrite` |
| Every `sync = true` write survives repeated simulated power losses | `DBPowerLoss.SyncedWritesSurviveAcrossRepeatedOutages` |
| The first synced write after `Open` survives (the new log's *name* is made durable) | `DBPowerLoss.TheFirstSyncedWriteAfterOpenSurvives` |
| Losing unsynced writes never loses a synced one or leaves an unopenable database | `DBPowerLoss.LosingUnsyncedWritesLeavesSyncedOnesIntact` |
| A batch is recovered all or nothing | `WriteBatch` is one log record; `Log.EveryTruncationOffsetYieldsACleanPrefix` |
| A torn write at the end of the log is discarded without error | `DBTest.TornTailIsDiscardedSilently`, `Log.EveryTruncationOffsetYieldsACleanPrefix` |
| Damage *inside* the log fails `Open` instead of silently dropping writes | `DBTest.CorruptionInsideTheLogFailsOpen` |
| After a failed write or sync, no later write is acknowledged | `DBPowerLoss.AFailedSyncStopsAllFurtherWrites` |
| Recovery never resurrects an old write underneath a newer synced one | `DBPowerLoss.RecoveryNeverResurrectsAWriteBehindANewerSyncedOne` |
| Power loss immediately after recovery loses nothing | `DBPowerLoss.DataSurvivesPowerLossRightAfterRecovery` |
| A clean `Close` makes unsynced writes durable | `DBPowerLoss.CleanCloseMakesUnsyncedWritesDurable` |
| Overwrites and deletes are recovered exactly, in order, after `SIGKILL` | `DBCrash.SigkillNeverLosesAnAcknowledgedWrite` (model-based) |
| Two processes can never open the same database | `PosixEnv.LockExcludesOtherProcessesUntilReleased`, `DBTest.SecondOpenOfTheSameDatabaseIsBusy` |

### How the tests know they are testing something

For each call that makes data durable, a **negative control** was run. The call was deleted, the
crash tests were rerun, and at least one had to fail (phase 3 journal):

| Deleted call | Tests that failed |
|---|---|
| `SyncDir` after creating a log | 3 power-loss tests |
| `fdatasync` for `sync = true` | 3 power-loss tests |
| the log writer's per-record `Flush` | the `SIGKILL` test |
| recovery's consolidation (entirely) | the resurrection test |
| consolidation's `fdatasync` | `DataSurvivesPowerLossRightAfterRecovery` |
| consolidation's `SyncDir` | the resurrection test |
| `Close`'s `fdatasync` | `CleanCloseMakesUnsyncedWritesDurable` |

## The power-loss model

Simulated power loss (`tests/support/fault_injection_env.hpp`) assumes the **worst case POSIX
allows**:

- bytes appended after a file's last successful `fdatasync` are lost;
- files created, renamed, or removed since the last `fsync` of their directory revert.

Real hardware can keep more than this, never less. Passing against the worst case therefore
covers every outcome a real power loss can produce.

## What is *not* guaranteed

- **Hardware that lies.** A disk whose write cache ignores flush commands can lose `fdatasync`ed
  data. No software can detect or prevent that.
- **Silent media corruption of old data.** Checksums *detect* it: every log record is
  CRC-32C protected. Nothing *repairs* it.
- **The directory's own creation.** If the database directory itself is new, its entry in its
  parent directory is not synced. Create the parent directory ahead of time for a guarantee from
  the very first write.
- **Filesystems without POSIX `fsync` semantics**, for example a WSL `/mnt/c` mount. Keep
  databases on a native Linux filesystem.

## Recovery policy

| What recovery finds | What happens |
|---|---|
| Nothing wrong | every log is replayed in order, then **consolidated**: rewritten into one new log, synced, and the old logs deleted |
| A record cut off at the very end of a log | discarded quietly: it was never acknowledged |
| Damage at the end of the *newest* log with no valid record after it | treated as a torn write (e.g. zero-filled blocks left by power loss), discarded, logged |
| A bad checksum, bad length, or undecodable batch followed by valid records | `Open` fails with `Corruption` and names the file |
| A batch whose sequence numbers were all recovered already | skipped as a duplicate (left by an interrupted consolidation) |
| The same, with `Options::ignore_wal_corruption = true` | the damaged region is skipped, and each skip is logged in `LOG` |
| An I/O error reading a log | `Open` fails, even in salvage mode |

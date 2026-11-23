# Limitations

What strata does not do, or does not do well, as of `v1.0.0-rc.1`. Each item says why, and
what to do about it. Nothing here is a bug that is known and unfixed; those would be issues.

## Scope

- **Linux only.** The `Env` is POSIX (`pread`, `fdatasync`, `flock`-style `LOCK`, directory
  fsync); on Windows, build and run inside WSL2. Other POSIX systems are untested.
- **One process per database.** The `LOCK` file refuses a second opener. There is no
  read-only or secondary mode, so `stratactl stats` and `serve` cannot attach to a database
  another process has open (see `docs/observability.md`).
- **Embedded, not a server.** No network protocol for data, no replication, no
  authentication. The stats endpoint is read-only and meant for loopback.
- **No transactions beyond an atomic `WriteBatch`**, no column families, no TTL, no merge
  operator, no range deletion, no user-supplied comparator in the public API.
- **Keys and values up to about 4 GiB** each (`WriteBatch::kMaxKeySize`, `kMaxValueSize`), and
  a whole value is held in memory to write or read it; there is no streaming API.

## Durability

- **`WriteOptions::sync` is off by default** (ADR-0005). An unsynced write survives a process
  crash, not power loss. Use `sync = true` for writes that must survive power loss;
  `docs/durability.md` states both guarantees exactly.
- **The guarantees assume a filesystem that honours `fsync` and directory `fsync`**, like ext4.
  `/mnt/c` under WSL2 (9P) does not; the tools refuse it for benchmarks, and data on it is not
  protected.
- **Recovery fails closed** on damage inside the log or the manifest, rather than dropping an
  acknowledged write silently. `Options::ignore_wal_corruption` exists for salvage.

## Performance

- **One mutex.** Writes are grouped (ADR-0006), and synced writes scale with threads, but a
  workload of many small unsynced writes from many threads is bounded by the mutex.
- **Leveled compaction with fixed ratios** (10× per level, 2 MiB tables, level 1 at 10 MiB);
  there is no universal or FIFO compaction and no rate limiter, so a heavy compaction competes
  with foreground I/O. Writes slow down at 8 level-0 tables and stop at 12.
- **Compression is off by default** (ADR-0009). On compressible data, snappy saves about a
  third of the disk for 14–18% of throughput; enable it with `Options::compression`.
- **No prefix Bloom filters or partitioned indexes**; the index and filter of every open table
  are held in memory (bounded by `max_open_tables`).
- **Benchmark figures are from one laptop under WSL2**, and most do not meet the project's
  own stability rule (CV ≤ 5%); they are labelled as such (ADR-0007). Re-measuring on a quiet
  host is the remaining gate for `v1.0.0`.

## Operations

- **The stats endpoint serves one request at a time** with a 2-second deadline per exchange,
  for an operator or a dashboard, not for scraping at high frequency. Binding a non-loopback
  address has no authentication or TLS.
- **Latency percentiles are sampled** (one call in 16 per thread); call counts are exact.
- **A database whose tables use a compression algorithm the running build leaves out** cannot
  read them (`NotSupported`), and a compaction that needs them stops writes. Build with every
  algorithm the database has used.
- **The first build needs network access** to fetch GoogleTest, zstd, and snappy (pinned
  commits); later builds do not.

## Verification

- **Soak: one hour, not 24.** The release gate asks for 24 hours; the release soak ran for one
  hour (by the user's choice) and is reported under that duration (ADR-0010).
- **Fuzzing: one hour per target**, with libFuzzer and ASan/UBSan, on a laptop. Before phase
  14 the fuzzers were built without coverage instrumentation of the library, so earlier,
  shorter fuzz runs were weaker than their reports implied (phase 14 journal).
- **Two lines of the compaction merge are not covered by tests**: the guard for a malformed
  internal key inside a table whose checksum is valid, which only a crafted table can reach.
  See the phase 15 journal.

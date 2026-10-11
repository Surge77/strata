# Benchmarks

This page explains how strata's performance is measured and what the measured
numbers are. No number here comes from anywhere but a run of `stratabench`,
with the machine, the method, and the variance stated next to it. If a number
is not stable enough to trust, the page says so instead of printing it as a
result.

- [Methodology](#methodology): the statistical protocol, the workloads, the
  open-loop latency harness, and how profiles are taken.
- [Results](#results): what the reference machine measured.
- [Reproducing a run](#reproducing-a-run)

## Methodology

### Statistical protocol

A benchmark round is one complete measurement, such as loading one million
keys into a fresh database. Rounds of the same benchmark vary, because the OS
schedules threads differently, because a background compaction starts in one
round and not the next, and because CPU frequency changes. One round tells
you nothing about this spread, so every figure goes through the same
protocol:

1. **Warm-up.** The first `--warmup` rounds (default 1) run and are thrown
   away. They fill the page cache, fault in the binary, and let the CPU
   frequency settle.
2. **Measured rounds.** At least 5 rounds (`--rounds`, default 5) are
   measured. A run with fewer rounds is marked `publishable: false` in its
   report and is not used on this page.
3. **Median.** The reported figure is the median of the measured rounds. One
   round that a compaction slowed down moves the mean, but not the median.
4. **Coefficient of variation.** The spread is reported as the coefficient
   of variation (CV): the sample standard deviation (divided by n - 1)
   divided by the mean. It is unitless, so throughput and latency spreads
   can be compared directly.
5. **Stability gate.** A figure is *stable* when its CV is at most 5%.
   `stratabench` exits with status 3 if any gated figure is unstable. An
   unstable figure is never published as a result. It is either measured
   again with more care (longer rounds, a quieter machine) or reported as
   unstable together with its CV.

Why 5%: two medians whose difference is smaller than their combined spread
cannot be told apart. With CV at most 5%, a change of about 10% or more is
real, and phase 12 needs exactly that to show that observability added no
measurable cost to the hot path.

**Comparing two builds.** Both builds run on the same machine in the same
session with the same arguments. Their runs alternate (A, B, A, B), so slow
drift such as thermal throttling affects both equally. A difference is
reported only when it is larger than the sum of the two CVs.

### What is gated

Every closed-loop throughput figure is gated. In the open-loop harness, the
achieved rate and the median response time are gated. Tail percentiles (p99,
p99.9, max) are reported with their CV but are not gated. A p99.9 taken from
a few seconds of samples rests on a few dozen values, so its spread mostly
reflects that small sample. Requiring 5% there would mean either very long
runs or quietly dropping the figure.

### Closed-loop workloads (throughput)

In a closed loop, each thread issues its next operation as soon as the
previous one returns. This measures the maximum rate the engine sustains. It
cannot measure latency under load, because a slow operation also delays the
operations that would have arrived during it (see the next section).

Keys are 16 decimal digits (`0000000000001234`) and values are 100 bytes
unless `--value_size` says otherwise. `--num` (default 1,000,000) is the
number of keys.

| Workload | Database | Operation | Figure |
|---|---|---|---|
| `fillseq` | fresh each round | `--num` unsynced Puts in key order | ops/s |
| `fillrandom` | fresh each round | `--num` unsynced Puts, uniformly random keys | ops/s |
| `fillsync` | fresh each round | `--num` / 1000 synced Puts (at least 100), random keys | ops/s |
| `readrandom` | shared, loaded | Gets of uniformly random existing keys | ops/s |
| `readzipf` | shared, loaded | Gets of existing keys, Zipfian (θ = 0.99) | ops/s |
| `readmissing` | shared, loaded | Gets of keys that do not exist | ops/s |
| `scanfull` | shared, loaded | one forward scan of the whole database | entries/s |
| `seekrandom` | shared, loaded | Seek to a random key, then read 100 entries | scans/s |
| `ycsb-a` | shared, loaded | 50% Get / 50% Put, Zipfian keys | ops/s |
| `ycsb-b` | shared, loaded | 95% Get / 5% Put, Zipfian keys | ops/s |

The **shared, loaded** database is built once per run. `--num` keys are
inserted in random order, the database is reopened, and its compactions are
waited out, so reads go through a realistic multi-level tree rather than one
memtable. The read-only workloads run first. `ycsb-a` and `ycsb-b` run last,
because their Puts overwrite existing keys and change the tree.

`--threads` (default 1) splits each round's operations across that many
threads. `scanfull` is always one thread.

**Zipfian keys** follow YCSB's scrambled Zipfian generator. A rank is drawn
with probability proportional to 1/rank^θ, then hashed onto the key space,
so the popular keys are spread across all tables instead of sitting in the
first one.

**Warm and cold cache.** By default the page cache is warm: the loaded
database is about 120 MB and fits in memory. With `--cold`, before each read
round the database is closed, every file in it is evicted from the page cache
with `posix_fadvise(POSIX_FADV_DONTNEED)` (no root needed), and the database
is reopened, which also empties the block cache. On WSL2 this evicts only
the Linux guest's cache. Windows may still hold the virtual disk's blocks in
its own cache, so a "cold" read on WSL2 can be served from host memory and
is faster than a real disk read.

### Open-loop latency harness

A closed loop understates latency under load. Suppose an operation stalls for
50 ms. The thread waits, and the operations that should have arrived during
those 50 ms are never sent. Each later operation is timed from when it was
actually sent, so the stall shows up as one slow sample instead of hundreds.
This error is called **coordinated omission**: the load generator coordinates
with the system it measures and leaves out exactly the samples that would
show the problem.

The open-loop harness fixes the arrival times in advance:

1. A schedule of `rate × duration` arrival times is generated before the run.
   The gaps between arrivals are exponentially distributed, which makes the
   arrivals a Poisson process (independent arrivals, as from many separate
   clients), seeded so that every run uses the same schedule.
2. `--workers` threads (default 4) take arrivals in order. A worker sleeps
   until the arrival's scheduled time, unless that time has already passed.
3. **Response time** is measured from the *scheduled* time to completion, so
   time an operation spent waiting because the workers were behind counts.
   **Service time** is measured from the actual start to completion. When the
   two diverge, operations are queueing.
4. The run is **saturated** if the achieved rate falls below 98% of the
   target. The report then labels its latencies as queue-dominated.

This models a server with a fixed pool of worker threads fed by independent
clients. Latencies are recorded in nanoseconds in strata's `Histogram`, whose
log-linear buckets keep every percentile within 6.25% of the true value.

**Target rates.** With `--rates=auto` (the default), the harness first
measures the closed-loop maximum for the same operation mix and worker count.
It then runs at 25%, 50%, 75%, and 90% of that maximum. Explicit rates can be
given as `--rates=10000,20000`.

**Harness floor.** The harness itself adds latency: a thread that sleeps
until a scheduled time wakes up late by however long the kernel takes to
schedule it. The `null` operation measures that delay: it runs the same
schedule against an operation that does nothing. The `null` response time at
the highest rate is reported next to every open-loop result. A strata latency
that is close to this floor is a measurement of the harness, not of strata.
The harness sets the thread's timer slack to 1 ns (`PR_SET_TIMERSLACK`) so
the kernel does not delay wake-ups by its default 50 µs.

### Environment record

Every report starts with the environment it ran in: CPU model, logical CPU
count, memory, kernel, whether the kernel is WSL, the compiler and build
type, the commit, and the filesystem type of the scratch directory.
`stratabench` refuses to run when the scratch directory is on a 9P mount
(`/mnt/c` under WSL), because that filesystem's `fsync` and caching behaviour
would make the numbers meaningless. Pass `--allow_any_fs` to override this
check, for example on a filesystem it does not recognise.

### Profiling

`scripts/bench.sh --profile <workload>` runs one workload under
`perf record -e cpu-clock --call-graph dwarf` and saves a `perf report`
sorted by symbol. `cpu-clock` is a software timer event. Hardware counters
such as cycles and cache misses are not available inside WSL2. With
`perf_event_paranoid = 2`, perf samples only user-space code, so time spent
in the kernel (system calls, page faults) is invisible to the profile. Use
the workload's wall-clock time to see it.

## Results

*Not yet recorded. Phase 11 records them here.*

## Reproducing a run

```bash
scripts/build.sh                                 # RelWithDebInfo into ~/build/strata
scripts/bench.sh                                 # full suite, results under ~/strata-bench/
scripts/bench.sh -- --workloads=readrandom --threads=4
scripts/bench.sh --open-loop                     # latency at 25/50/75/90% of max
scripts/bench.sh --profile readrandom            # perf profile of one workload
```

Each run writes `report.json`, `stdout.txt`, and (with `--profile`) a perf
report to a fresh directory under `~/strata-bench/`, named with the UTC time
and commit.

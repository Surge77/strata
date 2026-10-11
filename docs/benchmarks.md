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
| `cpu` | none | crc32c over a 256 KiB buffer (host-noise probe, below) | MB/s |
| `fillseq` | fresh each round | `--num` unsynced Puts in key order | ops/s |
| `fillrandom` | fresh each round | `--num` unsynced Puts, uniformly random keys | ops/s |
| `fillsync` | fresh each round | `--num` / 1000 synced Puts (at least 100), random keys | ops/s |
| `readrandom` | loaded | Gets of uniformly random existing keys | ops/s |
| `readzipf` | loaded | Gets of existing keys, Zipfian (θ = 0.99) | ops/s |
| `readmissing` | loaded | Gets of keys that do not exist | ops/s |
| `scanfull` | loaded | full forward scans of the whole database | entries/s |
| `seekrandom` | loaded | Seek to a random key, then read 100 entries | scans/s |
| `ycsb-a` | loaded, fresh copy each round | 50% Get / 50% Put, Zipfian keys | ops/s |
| `ycsb-b` | loaded, fresh copy each round | 95% Get / 5% Put, Zipfian keys | ops/s |

A fill round does a fixed amount of work. It is timed from the first Put to
the return of the last one, which measures *foreground ingest*. Flushes and
compactions still pending at that point are not waited for, so a fill figure
does not include the compaction debt it creates. Before the clock starts, the
filesystem is written back (`syncfs`), so the previous round's files cannot
be flushed during this round's fsyncs. A round on the loaded database
repeats its operation until at least `--min_round_s` (default 3 s) has
passed. The deadline is checked every 256 operations. Rounds of a fraction
of a second are skewed by any brief host interruption (see "What went wrong"
in the phase 11 journal), which is why Google Benchmark has the same
`min_time` rule.

The **loaded** database is built once per run. `--num` keys are inserted in
random order and its compactions are waited out, so reads go through a
realistic multi-level tree rather than one memtable. That state is kept as a
pristine copy. Workloads run against a work copy. Every round of a workload
that writes (`ycsb-a`, `ycsb-b`, the open-loop harness) starts from a fresh
copy of the pristine state, so each round is the same experiment, not a later
one on a tree the previous round grew. Copying is not timed.

`--threads` (default 1) sets the number of threads issuing operations in each
round. `cpu` and `scanfull` always use one thread.

**Zipfian keys** follow YCSB's Zipfian generator. A rank is drawn with
probability proportional to 1/rank^θ, then mapped onto the key space by
multiplying by a fixed prime modulo the key count. That map is a
permutation, so the popular keys are spread across all tables instead of
sitting in the first one, and no two ranks share a key.

**Host-noise probe.** The `cpu` workload times fixed CPU work, with no
database code and no I/O, through the same protocol. If it is stable while
an engine figure is not, CPU frequency scaling is not the cause, and the
disturbance is elsewhere (other processes, the host's disk, or the engine).

**Warm and cold cache.** By default the page cache is warm: the loaded
database is about 120 MB and fits in memory. With `--cold`, before each read
round the database is closed, every file in it is written back and evicted
from the page cache with `fdatasync` and `posix_fadvise(POSIX_FADV_DONTNEED)`
(no root needed), and the database is reopened, which also empties the block
cache. On WSL2 this evicts only
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

`scripts/bench.sh --profile <workload>` runs one round of one workload under
`perf record -e cpu-clock --call-graph dwarf` and saves `perf report` output
sorted by symbol, both flat and with call graphs. perf starts with sampling
disabled (`-D -1`). stratabench enables it through perf's control FIFOs only
while the measured workload runs, so a read workload's profile leaves out the
million-key load that built its database. `cpu-clock` is a software timer event. Hardware counters
such as cycles and cache misses are not available inside WSL2. With
`perf_event_paranoid = 2`, perf samples only user-space code, so time spent
in the kernel (system calls, page faults) is invisible to the profile. Use
the workload's wall-clock time to see it.

The dev machine has no root, so perf is not installed system-wide. It comes
from the Ubuntu archive, unpacked into the user's home:

```bash
cd "$(mktemp -d)"
apt-get download linux-tools-6.8.0-146-generic linux-tools-6.8.0-146 \
                 linux-tools-common libnuma1 libtraceevent1
for f in *.deb; do dpkg -x "$f" ~/.local/perf-root; done
# then run ~/.local/perf-root/usr/lib/linux-tools-6.8.0-146/perf with
# LD_LIBRARY_PATH=~/.local/perf-root/usr/lib/x86_64-linux-gnu, or set PERF=
# to a wrapper script that does so
```

## Results

> **Gate status: not met on this machine.** In the run below, 3 of the 10
> engine figures are stable (CV at most 5%), as is the host probe. The others are printed
> with their CV and flagged. They are measurements, not results, until they
> are repeated on a quieter host, and that repetition is a v1.0 release gate
> ([roadmap](roadmap.md)). Why this machine cannot hold 5% is in the
> [phase 11 journal](journal/phase-11-benchmarks.md#what-went-wrong).

**Reference machine.** Intel Core i5-12450H laptop (4 P-cores + 4 E-cores),
on AC power, Windows 11 "Best performance" power mode, WSL2 kernel
6.6.114.1 with 8 vCPUs and 10.7 GiB of memory, and the WSL VM pinned to the
P-cores (`ProcessorAffinity = 0xFF`). Scratch directory on ext4 inside the
VM. gcc 13.3, RelWithDebInfo (`-O2 -g`), no sanitizer. Engine at the phase
10 tag, which is unchanged since then (`src/` and `include/` are identical).
The closed-loop table comes from stratabench at commit `f763662`. The
open-loop table and the profiles come from `1015d35`, one commit before the
review fixes. Those fixes changed what the fill workloads time, not the
open-loop path or what perf samples.

### Closed-loop throughput

1,000,000 keys, 100-byte values, 1 thread, warm cache, 1 warm-up and 5
measured rounds, at least 3 s per loaded-database round. The loaded tree had
1, 4, and 49 tables in levels 0, 1, and 2.

| Workload | Median | CV | Stable |
|---|---:|---:|:-:|
| `cpu` (host probe) | 10,825 MB/s | 1.4% | yes |
| `fillseq` | 492,321 ops/s | 6.8% | no |
| `fillrandom` | 428,505 ops/s | 15.1% | no |
| `fillsync` | 820 ops/s | 2.3% | yes |
| `readrandom` | 260,121 ops/s | 8.4% | no |
| `readzipf` | 285,976 ops/s | 6.7% | no |
| `readmissing` | 2,275,671 ops/s | 3.5% | yes |
| `scanfull` | 4,894,324 entries/s | 21.9% | no |
| `seekrandom` | 37,179 scans/s | 4.6% | yes |
| `ycsb-a` | 322,151 ops/s | 31.2% | no |
| `ycsb-b` | 343,702 ops/s | 7.8% | no |

The figures that are stable here were unstable in other runs, and the
reverse. Across runs on the same machine, `fillseq` medians ranged from
492k to 762k ops/s, and the median of a figure moved by more than its own
CV. That is why none of these numbers counts as a result yet.

`fillsync` is bounded by the virtual disk's flush: about 1.2 ms per
acknowledged write on this machine. That flush time ranged from 1.0 ms to
4.8 ms between sessions on the same machine, so this figure says more about
the host's disk than about the engine.

### Open-loop latency

90% Gets and 10% Puts, Zipfian keys, 4 workers, 5 s per round, 1 warm-up and
5 measured rounds, each round on a fresh copy of the loaded database.
Medians across rounds, in microseconds. The closed-loop maximum for this mix
was 398,136 ops/s (CV 27.4%, unstable), and the four rates are 25/50/75/90%
of it.

| Offered ops/s | Achieved | Response p50 | p99 | p99.9 | Service p50 | Service p99 | |
|---:|---:|---:|---:|---:|---:|---:|---|
| 99,534 | 99,541 | 22.4 | 224 | 7,524 | 5.3 | 96 | |
| 199,068 | 198,976 | 21.7 | 62,348 | 64,758 | 4.8 | 107 | |
| 298,602 | 298,504 | 38.0 | 150,751 | 152,747 | 4.0 | 97 | saturated in 2 of 5 rounds |
| 358,323 | 258,240 | 819,611 | 1,866,207 | 1,935,072 | 4.8 | 143 | saturated |
| no-op at 358,323 | 358,379 | 6.8 | 49 | 94 | 0.1 | 0.3 | harness floor |

What it shows, at magnitudes far beyond the noise:

- **Service time never moves.** p99 stays near 100 µs at every rate. A
  closed-loop benchmark reports exactly this column and would call the
  engine's tail latency 100 µs.
- **Response time does.** At half the maximum rate, p99 is 62 ms: about 1 in
  100 requests waits behind a stall about 600 times longer than an
  operation. That is the coordinated-omission gap the harness exists to
  expose.
- **The stall is the WAL rotation.** When the memtable fills, the writer
  that rotates it creates the new log, fsyncs the directory, and fsyncs the
  retired log (up to 4 MB of unsynced data), all while holding the DB mutex
  (`DBImpl::NewLogFile`). Every Get and Put waits behind it. Puts timed
  individually show a p99.9 of about 300 µs and maxima of 7-39 ms, about
  one per rotation. A reads-only open-loop run at the same rate has no such
  tail after warm-up (p99 about 200 µs). The fix is a separate change with
  its own crash tests (see the phase 11 journal).
- **The harness floor is about 7 µs at p50** and 49 µs at p99. strata's
  medians of 22 µs are therefore about 15 µs of engine time.

### Where the time goes (perf, one round each)

| Workload | Top symbols (share of user-space samples) |
|---|---|
| `readrandom` | crc32c 12.3%, varint decoding 8.0%, block-cache hash lookup 7.8%, Bloom probe 7.1%, block seek 3.7%, malloc/free 5.2% |
| `fillrandom` | skiplist insert 10.2%, varint decoding 8.4%, merging iterator 3.6% (compaction), crc32c 3.6% |
| `ycsb-a` | varint decoding 11.4%, memtable Get 7.2%, skiplist insert 5.3%, block-cache lookup 4.2%, crc32c 4.2% |

`readrandom` reads a 120 MB tree through an 8 MB block cache, so most Gets
miss the cache. A miss costs a `pread` from the page cache, a crc32c check of
the block (12%), parsing it (varint decoding, block seek), a cache insert,
and an eviction (allocation and hash-table work, about 15% together). None
of this is pathological. It is what a cache a fifteenth the size of the
data costs. Varint decoding appears in all three profiles because every
block entry and memtable key is length-prefixed. It is the first thing to
look at if point reads need to get faster.

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

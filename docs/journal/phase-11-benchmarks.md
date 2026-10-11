# Phase 11: Benchmarks

**Branch:** `feature/phase-11-benchmarks`. Gate: documented methodology;
coefficient of variation ≤ 5%.

## Starting point

Phases 6, 7, 9, and 10 each had a one-off benchmark binary with its own
copy of a `Repeat()` loop, three rounds, the population deviation, and no
record of the machine. There was no workload suite, no latency measurement
under load, and no profile of where time goes.

## What was built

- **The statistical protocol** (`tools/stratabench/stats.*`): warm-up rounds
  discarded, at least five measured rounds, the median with its sample
  coefficient of variation, and *stable* meaning CV ≤ 5%. A run with fewer
  rounds, or from a Debug or sanitizer build, is marked not publishable.
- **Key choosers**: uniform, and Zipfian (θ = 0.99, Gray et al.) scrambled by
  a prime-multiplication permutation, so the hot keys spread over every
  table, and the hot set keeps exactly the size Zipf gives it.
- **An open-loop harness** (`open_loop.*`): a seeded Poisson schedule fixed in
  advance, with each operation timed from its *scheduled* arrival, so a stall
  is charged to every arrival that queued behind it. Service time is recorded
  alongside it. Saturation is flagged.
- **`stratabench`**: 11 closed-loop workloads (a `cpu` host-noise probe, three
  fills, three point-read kinds, full scans, short seeks, YCSB A and B), plus
  the open-loop harness at 25/50/75/90% of the measured maximum and a no-op
  pass that measures the harness's own floor. Every report records the CPU,
  kernel, WSL, compiler, build type, sanitizer, scratch filesystem, and
  commit. The tool refuses a 9P scratch directory (`/mnt/c`).
- **`scripts/bench.sh`**: one directory per run, named by UTC time and commit,
  holding `report.json`, the output, and with `--profile` a perf profile
  restricted to the measured workload.

## Decisions

- **The harness is a tested library under `tools/`**, built by default, not
  under `benchmarks/` (off by default). So CI compiles it, and its statistics,
  generators, and coordinated-omission handling have unit tests: 27 tests and
  a CLI smoke test.
- **Coordinated omission is tested, with a negative control.** One operation
  sleeps 200 ms. The test requires the response-time p90 to exceed 50 ms
  while the service-time p99.9 stays below 50 ms. Timing from the actual start
  instead of the scheduled time (what a closed loop does) fails it.
- **Mixed operations are pure functions of `(seed, index)`**, so one instance
  serves every open-loop worker without locks, and a schedule replays
  identically.
- **Profiles start disabled.** `perf record -D -1 --control fifo:…` is
  switched on and off by stratabench around the measured workload, so a read
  profile does not mostly show the million-key load before it.
- **perf without root.** WSL2's kernel has no matching `linux-tools` package
  and the machine has no root, so perf comes from the Ubuntu archive,
  unpacked with `dpkg -x`. WSL2 has no hardware PMU, so the profiles use
  `cpu-clock`. With `perf_event_paranoid = 2` they see user space only.

## Gate: methodology and CV ≤ 5%

**Methodology: met.** [docs/benchmarks.md](../benchmarks.md) was written
before the harness and revised as the runs showed it was wrong (see below).

**CV ≤ 5%: not met on this machine, recorded rather than lowered.** In the
final pinned run (stratabench commit `f763662`; the engine is identical to
the phase 10 tag), the host probe held at 1.4%, but only 3 of the 10 engine
figures were stable (`fillsync` 2.3%, `readmissing` 3.5%, `seekrandom`
4.6%). The others ran from 6.7% to 31.2%. Which figures passed changed from
run to run. Every run had the same failure mode: several rounds per figure
off by 10-30%, not one bad round. A MAD-based
robust spread was computed on the same data and failed too, so switching
the statistic would only have hidden the problem. It was not adopted. With
the user's agreement, phase 11 merged with the gate recorded as unmet, and a
re-measurement on a quiet host became a v1.0 release gate in the roadmap.

What the noise does not touch are the effects that are much larger than 5%.
The open-loop result below is a 100-600× effect.

## Profiling: where the time goes

**The tail is the WAL rotation.** At 50% of the maximum rate, with 10%
writes, the open-loop response p99 was 62 ms while the service p99 stayed
near 100 µs at every rate. Narrowing it down:

1. *Is it the harness or the VM?* A pure-CPU operation of the same length
   (7 µs spin) at the same rate had p99 of 100-150 µs. So the tail is not
   the harness or the VM.
2. *Reads or writes?* Reads only, after warm-up, gave p99 of about 200 µs.
   Writes timed individually showed p99.9 of about 300 µs, but maxima of
   7-39 ms, about one per memtable rotation. The write-stall counters
   (`write_slowdowns`, `write_stops`) stayed at zero.
3. *What does the rotating write do?* `MakeRoomForWrite` → `SwitchMemTable`
   → `NewLogFile` creates the new log, fsyncs the directory, and fsyncs the
   retired log (up to 4 MB unsynced), all under the DB mutex. Each Get also
   takes that mutex briefly, so all four workers stall. This contradicts the
   `DBImpl` class comment, which says the mutex is never held across I/O.

The sync of the retired log is required for correctness: a later synced
write must never survive power loss while earlier acknowledged writes are
lost. What is wrong is doing it under the mutex. The fix changes the
durability ordering, so it needs its own crash tests and before/after
numbers from stratabench. It is the next branch,
`fix/rotation-sync-outside-mutex`, by the user's decision, rather than
being folded into a measurement phase.

**CPU profiles** (perf, user space, one round each) show no pathology.
`readrandom` on a tree 15× the block cache is dominated by block-miss work:
crc32c verification 12%, varint decoding 8%, cache hash lookup 8%, Bloom
probes 7%. `fillrandom` is led by the skiplist insert (10%) and varint
decoding (8%). Varint decoding is in every profile's top three, which makes
it the first candidate if point reads need to get faster.

## Verification

`src/` and `include/` are unchanged since the phase 10 tag, so the
`fsync` negative control was not re-run. The `sync_negative_control.sh`
mutants are identical to phase 10's, where 11 of 11 were caught.

- **Suites:** plain, ASan/UBSan, and TSan each pass **435/435**. That is
  phase 10's 407, plus 27 harness unit tests and the stratabench CLI smoke
  test.
- **Coverage:** **96.9% of lines** and 98.4% of functions in `src/` and
  `include/`.
- **Format:** the clang-format check is clean.
- **Negative control for the harness:** timing open-loop operations from
  their actual start instead of their scheduled arrival makes the
  coordinated-omission test fail.
- **Note:** gcovr writes temporary `.gcov` files into its working directory.
  Run from the `/mnt/c` checkout, it failed with a permission error, so
  coverage is reported from the ext4 build directory with `--root` pointing
  at the source.

## Review

An independent review found nothing CRITICAL. Fixed:

- **HIGH: fills timed a Zipfian setup they never used.** `KeyChooser`
  always built a Zipfian generator, which costs ζ(1M), a million `pow`
  calls. `Fill` built its chooser inside the timed region, so every fill
  round, `fillseq` included, paid about 20-50 ms: a systematic 2-4% bias
  that a low CV cannot reveal. Choosers are now built before the clock
  starts, and only Zipfian choosers build the generator. The closed-loop
  table was re-measured after the fix.
- **MEDIUM:** a fill round could pay for the previous round's write-back
  (now `syncfs` first, and the docs say fill figures exclude the compaction
  debt they create); `nan`/`inf` flags reached undefined float-to-integer
  conversions; `--cold` ignored a failed `posix_fadvise`, and tmpfs or
  overlay scratch directories were accepted; `bench.sh` could label a stale
  binary with `HEAD`'s commit and lose the exit status when perf reporting
  failed.
- **LOW items:**
  - An unpublishable (one-round) run exited 0. It now exits 3.
  - The perf FIFOs were reopened for each command. They now stay open.
  - Operation 0's seed collided with the schedule's seed.
  - A two-item Zipfian chooser relied on bit-exact `pow` to avoid 0/0.
- **Accepted:**
  - `std::exit` from a worker thread on an engine error.
  - Two unit tests whose timing bounds a very slow host could trip.
  - A saturation check that can miss a short persistent queue.

## What went wrong

The first runs failed the gate. Each failure was a flaw in the harness or a
property of the machine, and only one was found by reasoning in advance.

1. **Rounds too short.** Fixed-count rounds took 0.2 s (`scanfull`), 0.4 s
   (`seekrandom`), and 0.5 s (`readmissing`). One brief host interruption
   swung a round by 40%. *Why?* The round length came from an operation
   count chosen for a 1M-key run, not from a time budget. *Fix:* rounds on
   the loaded database run for at least `--min_round_s` (3 s), the same rule
   as Google Benchmark's `min_time`.
2. **Writing rounds were different experiments.** `ycsb-a` fell from 346k to
   220k ops/s over five rounds, because each round's Puts grew the tree that
   the next round started from. *Why?* The protocol assumed rounds are
   independent, and nothing enforced it. *Fix:* the loaded state is kept as a
   pristine copy, and every writing round starts from a fresh copy of it.
   `ycsb-b`'s CV went from 10.7% to 1.1%.
3. **Setup I/O billed to the timed round.** The fresh copy left about 120 MB
   of dirty pages. On ext4 (`data=ordered`), the round's first `fsync`, the
   WAL rotation's, waited for a journal commit that also wrote the copy's
   data. The rotation holds the DB mutex, so YCSB rounds collapsed to 123k
   ops/s and the suite ran past ten minutes. *Fix:* `syncfs` after the copy,
   before the database opens.
4. **The machine.** The laptop was first measured on battery, which made CPU
   frequency erratic (`fillsync` CV 25%). On AC power, single rounds still
   fell 2-3× during bursts of host activity. A new `cpu` workload (fixed
   crc32c work, no database code, no I/O) showed the host itself was
   unstable: CV 6-9%, alternating between about 7 and 11 GB/s. The
   i5-12450H is hybrid, and Windows moved the WSL VM's virtual CPUs between
   P-cores and E-cores. Pinning the VM process to the P-cores (logical
   processors 0-7, `ProcessorAffinity = 0xFF`, which needs an elevated shell)
   brought the probe to 0.6%. The pin is lost whenever WSL restarts, so the
   final runs kept the VM alive.

*Lesson:* every one of these looked like engine noise. The host probe is
what separated "the engine is variable" from "the machine is variable".

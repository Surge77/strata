# Phase 12: Observability

**Branch:** `feature/phase-12-observability`.

## Goal

From the roadmap: metrics registry, structured logs, stats API, JSON HTTP endpoint.
Merge gate: **the endpoint is served, and the hot path shows no measurable regression.**

## Roles and methods

- **Principal engineer:** what must a dashboard (phase 13) be able to show without
  reading engine internals? That list became the metric, event, and JSON tables in
  `docs/observability.md`, written as a stable contract: renaming a metric is a breaking
  change.
- **Performance engineer:** every metric on a per-call path must cost a few nanoseconds
  and must not add a cache line that all reader threads share.
- **Security reviewer (STRIDE, narrowed to Information disclosure and DoS):** an HTTP
  listener inside a storage engine is new attack surface. What can a browser page, a
  slow client, or a flood of connections do to it?
- **Test engineer:** for each robustness claim in the docs, which test fails if the code
  that provides it is deleted?
- **Technical writer:** the doc tables are reference (Diátaxis); every row is checked
  against the code that produces it.

## Design decisions

| Decision | Alternatives | Why |
|---|---|---|
| Two homes for numbers: work already done under the DB mutex (flushes, compactions, commit groups) is counted in the existing `DBStats`; per-call work (Get, Write, iterators) goes through lock-free metrics | Everything in a registry; everything under the mutex | Under-mutex counting is free where the mutex is held anyway; per-call paths must not take a lock they did not take before |
| `Counter` and `ConcurrentHistogram` split over 8 per-thread shards, each on its own cache line | One atomic per metric | One shared atomic bumped by every reader turns into a cache line passed between cores on every `Get`. Found in review; see below |
| `get.calls` and `write.calls` are the latency histograms' counts | Separate counters | They counted exactly the same calls, so the counter was one more atomic per call for nothing |
| Latency samples in nanoseconds, reported as fractional microseconds | Whole microseconds | A memtable `Get` takes under 1 µs and recorded as 0. Found in review |
| The registry hands out references at registration; nothing is looked up by name on a hot path | Name lookups | A map lookup per `Get` would cost more than the metric |
| Block and table cache hits and misses counted under the LRU lock each lookup already takes | Atomics | Two plain increments, no new synchronisation |
| Events are logfmt lines in the info log *and* entries in memory: recovery events kept for the database's life, plus a ring of the last 256 | Log only; one ring | A dashboard needs events without parsing files, and what recovery did must survive hours of routine flushes pushing it out of a ring |
| JSON written by hand (`util/json`), invalid UTF-8 replaced with U+FFFD | A JSON library | Output only, about 150 lines; a dependency for that is not worth its supply-chain cost. File names can hold any bytes, so the output must still be valid JSON |
| HTTP server written by hand: one thread, one request at a time, one 2-second deadline per exchange, 8 KiB head cap | A framework; a thread per connection | It serves an operator or a dashboard once a second. One thread cannot be exhausted into spawning more, and the deadline bounds what a hostile client can hold |
| Bind `127.0.0.1` by default; on loopback, refuse any `Host` that is not loopback; no CORS headers | Bind all interfaces; no Host check | Without the Host check, a web page can rebind its own DNS name to 127.0.0.1 and read the endpoint through the visitor's browser (DNS rebinding) |
| `stratactl stats` / `serve` refuse to create a database | Open with defaults | Mistyping a path must not leave a fresh empty database behind |

## Steps

1. `feat(util)`: `Counter`, `ConcurrentHistogram` (with `Histogram::FromBuckets`),
   `MetricsRegistry`, `EventLog` with logfmt formatting, JSON helpers. Unit tests for
   each, including concurrent adds from more threads than shards.
2. `feat(db)`: `DB::GetStats` and `DBImpl::CollectStats`; Get/Write/iterator metrics;
   cache hit and miss counts; the engine's log lines become events; compaction progress;
   `StatsToJson`. Integration tests drive real flushes, compactions, and reopens.
3. `feat(obs)`: `StatsServer` with `/stats` and `/healthz`, hardened after a security
   review (non-blocking sockets, send deadline, `EMFILE` backoff, strict header names,
   `Host` required for HTTP/1.1, IPv4-mapped loopback recognised).
4. `feat(stratactl)`: `stats` and `serve`; `serve` shuts down cleanly on SIGINT/SIGTERM.
5. `docs`: every metric, event, the JSON schema, and the endpoint's security model.
6. Review fixes (below).
7. `perf(db)`: time one `Get` in 16 per thread (the first A/B: two clock reads were
   6–15% of `readmissing`).
8. `perf(util)`: an exclusive counter slot per thread, so an add is a load and a store,
   not a locked read-modify-write.
9. `perf(db)`: time one write in 16 per thread too, with `write.calls` an exact counter
   again (the final A/B, below).
10. `fix(obs)`: the second review's findings (below): shared counter slots spread like the
    exclusive ones, two new counter tests, and staggered sampling offsets.

`main` also took a fix before this phase merged: `fix/clang-build`
([journal](fix-clang-build.md)). Every clang job in CI had failed since phase 3, because GCC
accepted a use of `SkipList::Node` that clang rejects.

## Verification (final tree, `bafeb71`)

- **Suites:** plain, ASan/UBSan, and TSan each pass **514/514** with GCC 13, and so do
  ASan/UBSan (with the libFuzzer targets built) and TSan with clang 18. That is phase 11's
  435 plus the metrics, events, JSON, HTTP, stats, and server tests.
- **Coverage:** **96.9% of lines** (4629 of 4779) in `src/` and `include/`. The log reader
  and writer stay at 100%; the manifest code is at 99% and compaction's merge at 97%.
- **Format:** the clang-format check is clean.
- **Sync negative control:** **11 of 11** sites caught. The engine's sync calls moved
  lines but not count; the rotation fix keeps every one load-bearing.
- **The arena race:** with the old plain `size_t`, `StatsCanBePolledDuringSyncedWrites`
  fails under TSan; with the relaxed atomic it passes.
- **The endpoint is served:** `stratactl serve` answers `/stats` and `/healthz` in the CLI
  test, and the integration tests drive it over IPv4 and IPv6 loopback.

## Measurement

The gate is "no measurable hot-path regression". This laptop (i5-12450H under WSL2) could
not settle that end to end, so the answer rests on a direct measurement of the
instrumentation's per-call cost.

**End to end: two 6-pass interleaved A/Bs against `main`** (Release builds from `git
archive`, alternating which variant runs first; stratabench `readrandom`, `readmissing`,
`fillrandom` at 5 rounds, plus `concurrency_bench` at 5 rounds):

| Metric | Run 1 (writes fully timed) | Run 2 (writes sampled) |
|---|---|---|
| `readmissing`, 1 thread | −3.6% (passes +18 … −6) | −2.1% (passes +2 … −7) |
| unsynced writes, 1/2/4 threads | −4.1 / −5.6 / −4.7% | −2.0 / −3.4 / −4.0% |
| concurrent readers, 0 writers | **+10.8%**, all 6 passes faster | **−4.0%**, all 6 passes slower |

The reader rows are the tell. `Get` did not change between the two runs; only `Write`
did, and that reader row has no writers at all. Yet it moved by 15 points, consistently
across every pass. Differences of ±5–10% on this host therefore come from code layout and
the host, not from the change under test, and an end-to-end A/B here cannot resolve the
few nanoseconds in question.

**Directly: the instrumentation's cost per call** (a throwaway loop over the same
`Counter` and the same sampling decision, 200 million calls, 3 rounds, `-O2`):

| Cost per call | ns |
|---|---|
| one exclusive-slot `Counter::Add` | 0.0–0.5 |
| three adds (what a memtable `Get` does) | 0.4–0.9 |
| the 1-in-16 timing decision, clock reads included | 2.2–3.4 |
| two `steady_clock` reads on every call (the first design) | 33–42 |

So a `Get` now pays about 3–4 ns: under 1% of the cheapest `Get` (about 400 ns, a miss
that every Bloom filter rules out), and a write about 3 ns, roughly 0.3% of an unsynced
write. Run 1 is also why writes are now sampled. Timing every write cost the 33–42 ns
above, about 4% of an unsynced write, and it was the one cost large enough that the A/B
pointed at it.

**Verdict:** the gate is met by the per-call bound, not by the end-to-end A/B, which on
this host cannot detect a 1% difference either way. Phase 15's benchmark host
requirement (CV ≤ 5% on a quiet machine) is what would allow an end-to-end confirmation.

## Review

An independent review returned **Block**: two HIGH, four MEDIUM findings. All MEDIUM and
above are fixed.

- **HIGH: data race on the memtable's memory gauge.** A synced write inserts into the
  memtable with the DB mutex released (phase 9), so the leader can grow the arena while
  `CollectStats` reads `Arena::memory_usage_`, a plain `size_t`. No test polled stats
  while writers ran, so TSan never saw it. Fixed: the field is a relaxed atomic; its
  single writer does a load and a store, not a read-modify-write. New test
  `StatsCanBePolledDuringSyncedWrites`.
- **HIGH: `StatsServer` promised 503 after `DB::Close`, but `Close` frees the engine
  under the server thread** (use-after-free). The server calls the DB from another
  thread, so `db.hpp`'s rule (no `Close` racing other calls) applies. Fixed in the
  contract: stop the server first. The test that closed the DB under a running server
  now stops it and starts a new one on the closed handle.
- **MEDIUM: hot-path cost.** Every reader bumped the same `get.calls`, `get.found`, and
  table-read atomics, and `Add(0)` still paid a locked read-modify-write. Fixed:
  sharded counters, calls derived from histogram counts, zero adds skipped.
- **MEDIUM: tests that would pass with the code wrong.** The "client that never reads"
  test sent one request whose response fit in the socket buffers, so deleting the send
  deadline left it green. `SendAll` moved to `obs/http` and is tested on a socketpair
  whose peer never reads: it must give up at the deadline. Also added: `/healthz` 503
  with the engine's reason, IPv6 requests with the Host check, refused Gets and failed
  writes not counted, the `write_failed` event.
- **MEDIUM: `stratactl stats` opens the database read-write**, replaying and deleting
  logs. Documented: they are not inspection tools and cannot attach to a live database.
  A read-only open is left for later.
- **MEDIUM: latencies truncated to whole microseconds.** Fixed as above.

LOW findings fixed: a snapshot racing a shard's first sample could report `min > max`
(now repaired from the occupied buckets); `write.calls` counted refused writes (now only
accepted ones, so `write.calls / write.groups` stays the group-commit factor); a failed
log rotation stopped writes with no event (`log_rotation_failed`); an idle database
reported the last compaction's level; `accept` errors `ENOBUFS`/`ENOMEM` now back off;
a second Ctrl-C during `stratactl serve` shutdown now ends the process; doc wording on
logfmt quoting, lowercase methods, and the 408 reply's own deadline.

LOW findings left as they are, with the reason:

- `log_rotated` is written to the info log with the DB mutex held, once per memtable
  switch: one buffered `write(2)`, where the code before the rotation fix held the mutex
  across two fsyncs. Moving it out means returning the event from `NewLogFile`.
- `flush_finished` is recorded before the MANIFEST commit, so a failed commit shows
  `flush_finished` then `flush_failed`. Accurate (the table was written), if untidy.
- A request head ending in a bare `\n` followed by `\r\n` is not recognised and times
  out with 408. No real client sends it.

### Second review: the hot-path changes

The exclusive counter slots and sampled write timing came after the first review, so they
got their own. Verdict: **approve**, no CRITICAL or HIGH. The reviewer worked through the
slot handoff under the memory model (the release store of a freed slot synchronizes with
the next owner's acquiring exchange, so the count carries over and no add is lost) and
through `thread_local` destruction order. Three findings, all fixed:

- **MEDIUM: one shared slot for every thread past the eighth.** With 16 busy threads, 8
  would `fetch_add` one cache line per counter, three counters per `Get`: worse than the
  hashed shards this replaced, and invisible to a benchmark that stops at 8 threads.
  Counters now have 8 shared slots as well, picked round-robin.
- **LOW: the fallback after a slot's release had no test.** A `thread_local` destructor
  that runs after the release must add through a shared slot, because the freed slot may
  already belong to another thread. A test now checks exactly that, and fails when the
  fallback assignment is removed. The handoff test now releases each round's threads
  together, so slots are actually contended, under a reader that checks the total never
  decreases.
- **LOW: "the first call is timed" over-weights cold calls.** With short-lived threads,
  every thread's first (slowest) call was sampled. Threads now start their 1-in-16 count
  at staggered offsets, and the docs call the sample systematic rather than unbiased.

## What went wrong

- **Coverage reported 0 of 0 lines.** Five whys: gcovr found no data → it searched the
  `/mnt/c` source tree → its search path defaults to `--root` → the build tree is on ext4
  in `$HOME` (deliberately) → the command never named it. Fixed by passing the build
  directory as gcovr's search path.
- **The end-to-end A/B could not answer the question it was asked.** Two careful
  interleaved runs disagreed by 15 points on code that had not changed. The gate was
  settled by measuring the instrumentation's own cost, and the journal reports both.
- **The first gate run was killed** when Windows ran low on memory (Claude Code stops
  background jobs then). The rerun used 2 build jobs instead of 4.
- **The race the review found was invisible to TSan** because TSan only reports races
  that run. The suites exercised stats only from a single thread. The new test runs a
  poller against synced writers; with the old arena it fails under TSan (see
  Verification).

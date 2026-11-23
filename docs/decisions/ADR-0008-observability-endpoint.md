# ADR-0008: Observability through an in-engine, loopback-only, read-only HTTP endpoint

**Status:** accepted · **Date:** 2026-11-03 (phases 12–13), recorded 2026-11-23

## Context

An operator needs to see what a running engine is doing: rates, latency, the shape of the
tree, compactions, and what recovery did. A dashboard (phase 13) had to consume nothing but a
public API. The instrumentation must not slow the hot path measurably, and an HTTP listener in
a storage engine is new attack surface.

## Decision

1. **`DB::GetStats()` is the API**: counters, gauges, latency percentiles with their histogram
   buckets, the levels, the running compaction, and events (recovery events kept for the
   database's lifetime, plus a ring of recent ones). `StatsToJson` is its wire format, and
   metric names are a stable contract (`docs/observability.md`).
2. **The engine serves it itself** (`StatsServer`): one thread, one request at a time, a
   2-second deadline per exchange, `127.0.0.1` by default, a `Host` check against DNS
   rebinding, no CORS headers, read-only. The dashboard is served by the same server under a
   strict Content-Security-Policy, because a page from any other origin cannot read the
   endpoint.
3. **Hot-path metrics cost a few nanoseconds**: counters are per-thread exclusive slots (a
   load and a store, no locked instruction), and `Get` and `Write` time one call in 16 per
   thread. Work already done under the mutex is counted there.
4. **The dashboard reads only `/stats`**, and a test fails if any of its scripts requests
   anything else.

## Consequences

- About 3 ns per call of instrumentation (measured directly; phase 12).
- One slow client can delay others by about 2 seconds; the endpoint is for an operator or a
  dashboard once a second, not for traffic.
- Binding a non-loopback address serves anyone who can reach the port, with no
  authentication or TLS; `stratactl serve` warns, and the docs say to put a proxy in front.
- Latency counts are sampled (one call in 16), so a percentile is of the sample; counts of
  calls are exact.

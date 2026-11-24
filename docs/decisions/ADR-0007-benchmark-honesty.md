# ADR-0007: A benchmark figure is published only if it is stable, and gates are not lowered

**Status:** accepted · **Date:** 2026-10-11 (phase 11), recorded 2026-11-23

## Context

Storage-engine benchmarks are easy to make flattering: one lucky run, a closed loop that hides
queueing (coordinated omission), a warm cache presented as a cold one. Phase 11 set out a
protocol before writing the harness (`docs/benchmarks.md`) and then found that the reference
machine, a laptop running WSL2, could not hold most figures to it.

## Decision

1. **Every figure is the median of at least five measured rounds after a warm-up, reported
   with its coefficient of variation.** A figure is *stable* only at CV ≤ 5%; an unstable one
   is printed as UNSTABLE and is not quoted as a result.
2. **Latency comes from an open-loop harness** that times each operation from its scheduled
   arrival, not its actual start, so queueing shows up. A test with a negative control proves
   it: timing from the actual start makes it fail.
3. **A gate that is not met is recorded as not met.** It is not lowered, and the statistic is
   not swapped for one that passes (a MAD-based spread was computed on phase 11's data, failed
   too, and was not adopted).
4. **Comparisons that cannot be resolved say so.** When an end-to-end A/B cannot detect the
   effect in question (phase 12: a few nanoseconds per call against ±5–10% run-to-run swings),
   the question is answered by a direct measurement, and both results are reported.
5. **Every number in the README is traceable** to a recorded run on stated hardware.

## Consequences

- The release gate "benchmark suite re-measured with CV ≤ 5% on a quiet host" is not met on
  the reference laptop. With the user's agreement the release is tagged `v1.0.0-rc.1`, and
  `v1.0.0` waits for that re-measurement (ADR-0010).
- Effects much larger than the noise are still reported (the rotation fix's 100–600× p99
  change; compression's footprint), with every pass shown.

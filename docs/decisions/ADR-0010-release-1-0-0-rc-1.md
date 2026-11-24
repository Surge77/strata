# ADR-0010: Release as 1.0.0-rc.1, with two gates reported under what actually ran

**Status:** accepted · **Date:** 2026-11-24 (phase 15)

## Context

The roadmap's release gates for `v1.0.0` were written in phase 0 and revised in ADR-0004. At
the end of phase 15 every gate is met on the reference machine (a laptop, i5-12450H, WSL2)
except two that the machine, or the time available, cannot meet as written:

- **The benchmark suite at CV ≤ 5% on a quiet host.** Phase 11 could not hold most figures
  stable on this laptop and recorded the gate as unmet rather than lowering it (ADR-0007).
- **A 24-hour concurrent soak.** ADR-0004 allows a shorter run to be reported only under its
  real duration.

## Decision

1. **Tag the release `v1.0.0-rc.1`** (the library reports `1.0.0-rc.1`). `v1.0.0` is tagged
   when the benchmark suite has been re-measured at CV ≤ 5% on a quiet host, such as a
   dedicated cloud VM, with no engine change in between or with the gates re-run on any change.
2. **The soak runs for one hour**, by the user's choice, with periodic reopens, verification
   of every key against the writers' models, and the growth checks, and it is reported as
   one hour everywhere it is cited. A 24-hour run is welcome before `v1.0.0` but is not
   claimed.
3. **Every other gate is reported with its evidence** in the phase 15 journal, including the
   one line of the compaction merge left uncovered by tests (a defensive guard reachable only
   with a crafted table) and the one gcov attributes as uncovered although every compaction
   with output runs it.

## Consequences

- The version is honest about what has and has not been shown; nothing in the README claims
  more.
- Promoting to `v1.0.0` is a measurement, not development: re-run `scripts/bench.sh` on a
  quiet host and record the result.
- Users who build from the tag get the same engine `v1.0.0` would contain, unless the
  re-measurement finds something.

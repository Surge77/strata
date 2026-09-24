# ADR-0004: Revise the roadmap so durability is tested from the phase that creates it

**Status:** accepted · **Date:** 2026-09-24

## Context

The original roadmap (15 phases) was reviewed before phase 1 was finished, using a
**pre-mortem**: assume it is six months from now and strata failed; list the most likely
causes, then change the plan so each cause is prevented or detected early. The review was
done from the role of a principal engineer responsible for the engine's correctness claims,
not its feature list.

| # | Likely cause of failure | Evidence in the original plan |
|---|---|---|
| 1 | Durability bugs baked into the log and manifest formats before anything tests durability | Fault injection arrives in phase 8, after phases 2–7 already defined every on-disk format and every `fsync` call |
| 2 | "Thread-safe" retrofitted late, so phases 3–8 build on single-threaded assumptions | Concurrency is phase 9; nothing says the DB is safe to share before then |
| 3 | Fuzzing never happens, because the dev machine cannot run libFuzzer | The only local compiler is GCC 13; libFuzzer needs Clang |
| 4 | Compression phase blocked on system packages | Building here has no root, so `apt install libzstd-dev` is unavailable |
| 5 | Release gates quietly lowered at the end, so README numbers stop being honest | 24-hour soak and 1-hour-per-fuzzer runs cannot be performed during an interactive session |
| 6 | On-disk formats drift from their documentation | `docs/format/` is written "in phases 2, 4, 5" but not required before the code |

## Decision

1. **Fault injection moves from phase 8 to phase 2.** A `FaultInjectionEnv` that models power
   loss (bytes written but not `Sync`ed are lost; files created or renamed since the last
   `SyncDir` of their directory disappear) is the first deliverable of phase 2. Every phase
   that adds an `fsync`, `SyncDir`, or a durable format (2, 3, 5, 7) must ship at least one
   power-loss test for it. Phase 8 keeps the *systematic* crash matrix, model-based restart
   testing, disk-full/EIO injection, and the negative-control gate.
2. **The DB is thread-safe from phase 3.** Phase 3 serialises writers and reads under one
   mutex. That is slow but correct. Phase 9 then makes it fast (group commit, narrower critical
   sections, lock-free reads of immutable state) with TSan and the existing tests as the
   regression net. Correctness first, then performance, never the reverse.
3. **Fuzz harnesses are portable.** Each harness implements `LLVMFuzzerTestOneInput`. Under
   Clang they link against libFuzzer; under GCC they link a small driver that replays a seed
   corpus and then runs seeded random mutations for a fixed time. Harnesses land in the phase
   that introduces each untrusted-input reader (log in 2, SSTable in 4, manifest in 5), not
   later.
4. **Third-party codecs are fetched, not installed.** zstd and snappy come in through
   `FetchContent` pinned to exact release tags, the same way GoogleTest already does. The build
   stays reproducible with no system packages beyond a compiler and CMake.
5. **Long-duration gates are measured honestly.** The soak and fuzz harnesses take a duration
   argument. The full-length commands (24 h soak, 1 h per fuzzer) are documented and wired
   into a manually-dispatched CI job. Any run performed with a shorter duration is recorded
   with its actual duration in `docs/benchmarks.md` and `docs/limitations.md`. Nothing claims
   "24 hours" unless it ran for 24 hours.
6. **Formats are specified before they are coded.** A phase that introduces an on-disk
   structure commits its `docs/format/*.md` specification first, and the implementation is
   reviewed against it.
7. **Every phase gets a build journal entry** in `docs/journal/`, recording the roles taken,
   the method used, the decisions made, and the verification evidence, so that a reader can
   follow how the engine was built and not only what it is.

## Consequences

- Phases 2, 3, 5, and 7 each grow by roughly one test file. In exchange, a durability bug is
  found in the phase that introduced it, when its cause is still in working memory.
- Phase 3 performance is poor under concurrent writers by design. The phase 3 README section
  says so.
- The GCC fuzz driver is weaker than libFuzzer (no coverage guidance). It still catches the
  crash class that matters most for decoders: out-of-bounds reads on malformed length fields.
  Coverage-guided runs remain the CI gate.
- The local development machine cannot run Clang, clang-tidy, or libFuzzer. Those checks run
  in CI only, and the journal says which checks ran where.

**Revisit if** the development environment gains root access or a Clang toolchain, at which
point the libFuzzer and clang-tidy checks should also run locally before each merge.

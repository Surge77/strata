# Build journal

This directory records **how** strata was built: for every phase, which decisions were made,
by which reasoning method, and what evidence showed the result was correct. The ADRs in
`docs/decisions/` record the few decisions that shape the whole engine. This journal records
everything else, in order, so another engineer can retrace the work step by step.

## How each entry is written

Every phase entry follows the same shape:

1. **Goal**: what the phase must prove, copied from the roadmap merge gate.
2. **Roles and methods**: which expert perspective was taken for each part of the work, and
   which structured method was used.
3. **Design decisions**: each non-obvious choice, the alternatives considered, and why the
   choice won.
4. **Steps**: one line per commit, in order, with what it added and how it was tested.
5. **Verification**: the commands run and their results (tests, sanitizers, coverage).
6. **What went wrong**: bugs found, wrong turns, and how they were resolved. Not omitted when
   embarrassing: this is the most useful section for a reader.

## Roles

A single author building a storage engine has to keep switching viewpoints, because each role
catches a different class of mistake. The roles used, and what each is for:

| Role | Asks | Catches |
|---|---|---|
| **Principal engineer** | Is this the right thing to build, in the right order? | Plans that defer risk to the end |
| **Storage architect** | What is the on-disk format, and what invariant does each byte protect? | Formats that cannot be recovered or extended |
| **Senior C++ engineer** | Is this correct, minimal, and idiomatic C++20? | Lifetime bugs, UB, needless abstraction |
| **Reliability engineer** | What happens if the power fails *here*? | Missing `fsync`, torn writes, unsafe renames |
| **Security reviewer** | What does a malicious or corrupt input do? | Unchecked lengths, integer overflow, unbounded allocation |
| **Test engineer** | What would make this test pass while the code is wrong? | Tests that assert nothing, happy-path-only suites |
| **Performance engineer** | What does it cost, and how do we know? | Unmeasured claims, benchmark noise |
| **Technical writer** | Can a newcomer follow this without the author in the room? | Docs that describe intent instead of behaviour |

## Methods

| Method | Used for | Shape |
|---|---|---|
| **Pre-mortem** | Plan review | "It failed; why?" → list causes → change the plan so each is prevented or detected early |
| **ADR** (Nygard) | Engine-wide decisions | Context → Decision → Consequences → Revisit-if |
| **Spec-first** | On-disk formats | Write `docs/format/*.md` before the code, then review the code against it |
| **TDD** (red → green → refactor) | Every component | A failing test that states the behaviour, the smallest code that passes, then cleanup |
| **FMEA** (failure mode and effects analysis) | Durability code | For each write/`fsync`/rename: failure mode, effect, detection, test |
| **STRIDE, narrowed to Tampering and DoS** | Parsers of on-disk bytes | Treat every file as attacker-controlled; fuzz it |
| **Differential testing** | Anything with a simple reference model | Compare against `std::map` on random operation sequences |
| **Negative control** | Durability tests | Break the code on purpose (e.g. delete an `fsync`) and confirm a test fails |
| **Five whys** | Every bug in "What went wrong" | Ask why until the answer is a design or process cause |
| **Diátaxis** | User-facing docs | Keep tutorial, how-to, reference, and explanation separate |

## Entries

| Phase | Entry |
|---|---|
| 0 | foundation: predates this journal; see the phase-0 commits and ADR-0001 |
| 1 | [primitives](phase-01-primitives.md) |
| 2 | [log](phase-02-log.md) |
| 3 | [memtable and the first database](phase-03-memtable.md) |
| 4 | [SSTables](phase-04-sstable.md) |
| 5 | [Versions, MANIFEST and flush](phase-05-version-flush.md) |
| 6 | [Read path](phase-06-read-path.md) |
| 7 | [Compaction](phase-07-compaction.md) |
| 8 | [Crash tests](phase-08-crash-tests.md) |

# strata

An embeddable LSM-tree key-value storage engine written from first principles in C++20.

```cpp
#include <strata/db.hpp>

strata::DB db;
strata::Status s = strata::DB::Open({.path = "./data"}, &db);

db.Put("user:101", "rahul");
auto value = db.Get("user:101");
db.Delete("user:101");
```

> **Status: Phase 0 of 15 — engineering foundation.**
> The build, test, CI, and tooling skeleton are in place. The engine itself is being
> built one phase at a time; see [the roadmap](docs/roadmap.md). Nothing in this
> README claims a capability that is not yet implemented and tested.

---

## Why this exists

Almost every application sits on a storage engine, and almost no application developer
can say what happens between `put()` returning and the bytes being safe on disk. strata
is an attempt to answer that question by building the thing, not by reading about it.

Every component in this engine exists because of a specific constraint:

| Constraint | Mechanism |
|---|---|
| A write must survive `SIGKILL` | write-ahead log, `fdatasync`, replay on open |
| Writes must not pay for a random disk seek | in-memory ordered memtable |
| Memory is finite | flush the memtable to an immutable sorted file (SSTable) |
| Finding a key must not scan every file | sparse index + Bloom filter per SSTable |
| Files and dead versions accumulate | leveled compaction |
| A restart must know which files are the database | MANIFEST log + atomic `CURRENT` publish |
| Readers must not fault on a file compaction just deleted | refcounted immutable versions |
| Many threads, one disk | group-commit writer queue, lock-free reads of immutable state |

## Architecture

```
                    put(k,v)                          get(k)
                       │                                 │
                       ▼                                 ▼
                  WriteBatch                       active memtable
                       │                                 │ miss
                 WAL append                              ▼
                       │                        immutable memtables
              ── durability boundary ──                  │ miss
                       │                                 ▼
                       ▼                     ┌─ Bloom filter ─ definitely absent ─┐
                 active memtable             │        │ maybe present             │
                       │ full                │        ▼                           │
                       ▼                     │   sparse index                     │
              immutable memtable             │        ▼                           │
                       │ background flush    │    data block                      │
                       ▼                     └────────┬───────────────────────────┘
                    SSTable ◄──────────────────────────┘
                       │
                  compaction ──► SSTable (fewer files, no dead versions)
                       │
                       ▼
                   filesystem
```

Cross-cutting: MANIFEST/versioning, checksums on every on-disk structure, crash recovery,
metrics, and a fault-injection layer used to prove the durability claims above.

## Building

Linux only for v1.0. On Windows, build inside WSL2.

```bash
sudo apt install -y cmake ninja-build g++-13 clang-18 libzstd-dev libsnappy-dev

scripts/build.sh              # RelWithDebInfo into ~/build/strata
scripts/test.sh               # ctest
scripts/build.sh --asan       # address + undefined sanitizers
scripts/build.sh --tsan       # thread sanitizer
```

The build tree and all test data live on ext4 (`$HOME`), never on a `/mnt/c` 9P mount:
that filesystem does not provide the `fsync` semantics this engine's correctness tests
depend on, and it would make every benchmark number meaningless.

## Design documents

| Document | Contents |
|---|---|
| [`docs/roadmap.md`](docs/roadmap.md) | the 15 phases and what each one must prove |
| `docs/architecture.md` | component boundaries and data flow *(phase 5)* |
| `docs/durability.md` | exactly what "durable" means here, and what it does not *(phase 3)* |
| `docs/format/` | byte-level on-disk format specifications *(phases 2, 4, 5)* |
| `docs/decisions/` | architecture decision records |
| `docs/benchmarks.md` | measured results with full methodology *(phase 11)* |
| `docs/limitations.md` | what this engine does not do *(phase 15)* |

## Non-goals

Not a SQL database. Not distributed. Not a RocksDB replacement. No performance number
appears in this repository unless it was measured on a stated machine with a stated
method, and no durability guarantee is claimed unless a fault-injection test proves it.

## License

Apache-2.0. See [LICENSE](LICENSE).

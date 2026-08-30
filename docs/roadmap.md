# Roadmap

Fifteen phases. Each is a branch, a pull request, and a tag. A phase is not done when the
happy path works — it is done when its **merge gate** below passes.

Every phase merges only if: tests pass, sanitizer builds are clean, line coverage is at
least 80%, the relevant documentation is updated, and a benchmark is recorded where the
phase touches a performance-sensitive path.

| # | Phase | Deliverable | Merge gate |
|---|---|---|---|
| 0 | foundation | CMake/Ninja, warnings-as-errors, clang-format/tidy, GoogleTest, CI matrix, scripts, `stratactl` skeleton | `cmake --build && ctest` green in CI |
| 1 | primitives | `Slice`, `Status`, `Result<T>`, varint coding, crc32c, `Arena`, `Env`/`PosixEnv`, `LOCK` file, logging, `Histogram` | crc32c matches RFC 3720 vectors |
| 2 | log | Block-framed log writer/reader, `WriteBatch` encoding, corruption resynchronization | log-reader fuzzer clean; every truncation offset handled |
| 3 | memtable | Internal keys and comparator, arena skiplist, `Open`/`Put`/`Get`/`Delete`, WAL replay | `put → kill → reopen → get`; differential test against `std::map` |
| 4 | sstable | Block builder/reader with restart points, table builder/reader, footer, per-block checksums | round-trip plus corrupt-every-byte tests |
| 5 | version + flush | `VersionEdit`/`VersionSet`/MANIFEST/`CURRENT`, refcounted versions, `SyncDir`, background flush, obsolete-file GC | crash at each publish point yields a valid database |
| 6 | read path | Bloom filter, sparse index, table cache, block cache, two-level iterator | measured negative-lookup benchmark, filter on versus off |
| 7 | compaction | Snapshots, compaction picker, merge job, tombstone drop rule, write stalls | `CompactionDoesNotResurrectDeletedKey`; write amplification reported |
| 8 | crash tests | `FaultInjectionEnv`, crash matrix, model-based restart testing, disk-full and EIO injection | removing any single `fsync` **must** make a test fail |
| 9 | concurrency | Group-commit writer queue, narrowed critical sections, thread-sanitizer build | TSan clean; 1/2/4/8-thread scaling curve published |
| 10 | iterators | Merging iterator, bounded range scans, snapshot reads, public `WriteBatch` | scans match the reference model under concurrent writes |
| 11 | benchmarks | Workload suite, open-loop latency harness, statistical protocol, profiling | documented methodology; coefficient of variation ≤ 5% |
| 12 | observability | Metrics registry, structured logs, stats API, JSON HTTP endpoint | endpoint served; no measurable hot-path regression |
| 13 | dashboard | LSM level view, compaction progress, latency charts, recovery events | dashboard consumes only the JSON API |
| 14 | compression | Pluggable compressor, zstd and snappy | before/after footprint, throughput, and p99 |
| 15 | release | ADRs, limitations, demo script, soak test, `v1.0.0` | all release gates met |

Phases 0–3 produce a crash-safe persistent key-value store. Phases 0–7 produce the complete
LSM engine. Phases 8–15 are what make "production-grade" a defensible claim rather than a
word in a README.

## Release gates for v1.0.0

- Zero AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer findings.
- Line coverage ≥ 80% overall; 100% on the log reader/writer, manifest publish path, and
  compaction merge logic.
- Every crash-matrix point recovers to a state consistent with the reference model.
- **Negative control:** deleting any single `fsync` or `SyncDir` call causes a crash test to
  fail. If it does not, the crash tests are not testing durability.
- 24-hour concurrent soak with no leak, no unbounded memtable growth, no descriptor growth.
- Fuzzers clean for one hour each on the log reader, SSTable reader, and manifest reader.
- Every number in the README traceable to a reproducible run on stated hardware.

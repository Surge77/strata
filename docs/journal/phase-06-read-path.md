# Phase 6: Read path

**Branch:** `feature/phase-06-read-path`. Gate: measure negative lookups with
the Bloom filter on and off.

> **Status: implementation and gate complete (2026-10-04); merge and tag pending.**

## Starting point

Phase 4 already supplied the sparse index and two-level table iterator.
Phase 5 supplied a thread-safe table cache without a size bound. Phase 6 adds
the filter and bounded caches, then measures their effect.

## Decisions so far

- The first filter is one Bloom filter per table. It hashes user keys, not
  internal keys, so every sequence version of a key gives the same answer.
- The filter is optional. Old tables remain readable, and a missing filter
  falls back to an ordinary index and data-block lookup.
- `Table::Seek` keeps its lower-bound contract. Only exact user-key lookups
  consult the filter. A v1 filter is written and used only with the bytewise
  user comparator, since raw bytes are not safe for a comparator that treats
  different strings as equal.
- Its exact bytes and metaindex entry are specified in
  [the filter format](../format/bloom_filter.md). Invalid filter bytes fail
  table open with `Corruption` rather than allowing a false negative.
- One `LruCache` template backs both caches: open tables are charged 1 each
  (`Options::max_open_tables`, default 1000) and data blocks are charged their
  byte size (`Options::block_cache_size`, default 8 MiB, 0 disables it).
- Values are `shared_ptr`s, so eviction drops only the cache's reference.
  A lookup keeps its table or block alive until it finishes. That is the whole
  answer to "evict without deleting files held by readers": the table cache
  never deletes files. `DeleteObsoleteFiles` only removes files that no live
  `Version` references, so a table closed by the LRU can always be reopened.
- Blocks are keyed by (table file number, block offset). File numbers only
  increase within a process (`MarkFileNumberUsed` never lowers the counter),
  so a deleted table's blocks are left to age out instead of being purged.
- Only checksum-verified, parsed blocks enter the cache, so a hit can skip
  both the read and the checksum.
- `Options::bloom_filter` (default on) exists so the gate can be measured
  through the database. Tables written with either setting stay readable.

## Verification so far

- The Bloom filter unit tests cover inserted keys, binary and empty keys,
  negative lookups, invalid encodings, and stable on-disk hash vectors.
- Table tests verify lower-bound seek behavior, comparator safety, and that a
  malformed filter with a valid checksum fails open.
- A database-level test confirms stored keys remain visible and 100 missing
  keys cause fewer than 20 SSTable reads.
- Plain and ASan/UBSan suites pass **303/303**. Targeted TSan tests pass and
  the format check is clean. The SSTable fuzzer smoke test includes a filtered
  table seed. An independent review's two HIGH/MEDIUM findings were fixed and
  re-reviewed.
- Cache tests: 10 `LruCache` unit tests (LRU order, charge-based eviction,
  oversize entries, zero capacity, duplicate insert, value outliving eviction,
  concurrent use), 4 table-level block cache tests (hits, no cache, two tables
  with identical layouts sharing one cache, an iterator outliving its block's
  eviction), and 4 database tests that count real SSTable opens and reads.
- With the filter off, every database miss reads a data block (new test).
- Plain, ASan/UBSan, and TSan suites each pass **322/322**. Coverage is
  **97.7% of lines** and 99.5% of functions under `src/` and `include/`
  (gate: 80%); changed files are 92-100%.
- An independent C++ review found no CRITICAL or HIGH issues. Fixed:
  `LruCache::Insert` was not exception-safe (a failed index insert left an
  uncounted list entry whose later eviction would wrap `usage_`); the
  benchmark mixed statistics from different samples and timed every lookup
  inside the throughput figure; a missing `<iterator>` include; and the
  concurrent cache test did not check that returned values match their keys.
- Accepted for later: the single cache mutex will limit read scaling (phase 9
  owns concurrency and can shard it), and the block charge counts block bytes
  but not per-entry overhead (about 100 bytes), so small blocks can overshoot
  `block_cache_size` slightly.

## Gate: negative-lookup benchmark

`benchmarks/negative_lookup_bench` (build with
`-DSTRATA_BUILD_BENCHMARKS=ON`). 200,000 keys are written in shuffled order
with a 1 MiB write buffer, giving 32 level-0 tables that each span the whole
key range. Then 100,000 lookups of absent keys run, after a 10% warm-up, in 5
rounds interleaved across the four configurations. Mean and CV come from a
throughput pass timed once around the whole batch. Percentiles come from a
separate pass that times each lookup, and they include a 35 ns clock-read
pair. All statistics are pooled over the 5 rounds. The OS page cache is warm,
so "block cache off" is a `pread` served from memory, not from disk.

Machine: Intel Core i5-12450H (8 logical CPUs), 10 GiB RAM, WSL2 kernel
6.6.114.1, Ubuntu 24.04, g++ 13.3.0 `-O2` Release, database on ext4. Run on
2026-10-04.

| Configuration | Mean ns | p50 ns | p99 ns | SSTable reads per lookup | Run CV |
|---|---:|---:|---:|---:|---:|
| filter on, block cache on | 3,347 | 2,289 | 12,817 | 0.14 | 2.0% |
| filter off, block cache on | 86,794 | 88,730 | 256,895 | 20.00 | 2.5% |
| filter on, block cache off | 3,279 | 2,320 | 10,204 | 0.26 | 3.2% |
| filter off, block cache off | 82,647 | 75,896 | 214,641 | 32.06 | 1.6% |

- The filter makes misses about **25x faster** and cuts SSTable reads per
  miss from 20-32 to 0.14-0.26. Every row's run-to-run CV is within 5%. With 32 tables, 0.26 reads per lookup is
  about 0.8% false positives per table, which matches a 10-bits-per-key
  filter.
- Without the filter, the 8 MiB block cache made misses *slower* (87 vs
  83 us). The 32 tables' data blocks do not fit, so the cache churns: it
  evicts and inserts on almost every lookup and still saves only a third of
  the reads. A cache helps a working set that fits, not a scan-like miss
  pattern; the filter is what fixes misses.

## What went wrong

1. Putting the Bloom check inside `Table::Seek` broke its lower-bound contract:
   a query between two stored keys returned no next key. The filter now runs
   only for exact user-key lookups in `Version::Get`.
2. Hashing raw user-key bytes is unsafe with a comparator that equates
   different bytes. The v1 writer now requires the bytewise user comparator,
   and a reader with another comparator ignores this filter.
3. The first table-cache test expected exactly one open per table after
   reopening, but got 5 of 6. Opening the database turns the write-ahead log
   into a table and opens it to verify it, so one table was already cached.
   The tests now warm up with one pass and count only the second.
4. An LRU test expected one large insert to evict two entries when evicting
   one was already enough (3 + 3 + 6 = 12 against a capacity of 10, and one
   eviction leaves 9). The implementation was right. The test now makes a
   single insert displace three entries.
5. GCC's `-Wnull-dereference` rejected `*handle` in tests even after an
   `ASSERT_NE`; tests compare through a small null-safe helper instead.

## Next

Merge this branch into `main` with `git merge --no-ff`, tag `phase-06`, then
start phase 7 (compaction).

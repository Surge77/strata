# Phase 6: Read path

**Branch:** `feature/phase-06-read-path`. Gate: measure negative lookups with
the Bloom filter on and off. Status: in progress.

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

## What went wrong

1. Putting the Bloom check inside `Table::Seek` broke its lower-bound contract:
   a query between two stored keys returned no next key. The filter now runs
   only for exact user-key lookups in `Version::Get`.
2. Hashing raw user-key bytes is unsafe with a comparator that equates
   different bytes. The v1 writer now requires the bytewise user comparator,
   and a reader with another comparator ignores this filter.

## Remaining

1. Bound the table cache and add a block cache without deleting files held by
   readers.
2. Benchmark negative lookups with the filter on and off, recording the
   machine, workload, and measured results.
3. Run the full TSan and coverage gates after the caches land; review and fix
   findings before merge.

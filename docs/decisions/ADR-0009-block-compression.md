# ADR-0009: Per-block compression with snappy or zstd, off by default

**Status:** accepted · **Date:** 2026-11-23 (phase 14)

## Context

The SSTable format reserved a compression byte in every block trailer from phase 4. Phase 14
had to fill it with zstd and snappy, pinned via `FetchContent`, which are the first
third-party code in the engine's runtime. Decompression reads bytes from disk, which a damaged
or crafted file controls.

## Decision

1. **Each block names its own encoding** (0 none, 1 raw snappy, 2 one zstd frame). The reader
   needs no setting, and a database can change `Options::compression` at any time and still
   read every table it ever wrote.
2. **A block is stored compressed only if that saves an eighth**, so incompressible data costs
   nothing on reads. The filter block is never compressed.
3. **Decompression is bounded before allocation**: no block over 4 MiB is compressed, and a
   compressed block claiming to expand past 4 MiB, or to nothing, is `Corruption`. The
   checksum is verified before any decompression. Output must be exactly the claimed length.
4. **Dependencies are pinned to commits** (zstd v1.5.7 `f8745da6`, snappy 1.3.1 `9c28114a`),
   built minimal (no legacy zstd decoders), instrumented with the engine's sanitizers, and
   fuzzed directly (`block_decompress_fuzz`). Each can be left out of a build.
5. **The default is `Compression::kNone`.**

## Consequences

- Measured on half-compressible values: snappy −34% footprint for −14 to −18% throughput,
  zstd −43% for −44 to −52%; p99 at a moderate fixed rate unchanged (phase 14 journal).
- Off by default means no surprise cost for anyone upgrading, and one line to enable it. A
  deployment that wants smaller disks should turn on snappy.
- A database whose tables use an algorithm a later build leaves out cannot read them
  (`NotSupported`), and a compaction that needs them stops writes; build with every
  algorithm a database has used.
- The build needs network access once, on the first configure.

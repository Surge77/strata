# ADR-0001: LSM-tree rather than B-tree

**Status:** accepted · **Date:** 2026-08-30

## Context

The engine needs an on-disk structure for an ordered key-value map that survives process
death. The two mature choices are a B+tree (as used by LMDB, BoltDB, and most relational
engines) and a log-structured merge tree (as used by LevelDB, RocksDB, and Cassandra).

The project's stated purpose is to make the write path, durability boundary, crash recovery,
and background work of a storage engine visible and explainable.

## Decision

Use an LSM-tree.

## Consequences

**Why it fits the goal.** An LSM-tree makes each subsystem correspond to a distinct systems
constraint that can be motivated, implemented, and measured on its own: a WAL for durability,
a memtable for write latency, immutable SSTables for persistence, Bloom filters and a sparse
index for lookup cost, and compaction for space and read amplification. A B+tree collapses
most of this into page management and a single tree-modification algorithm, which is subtler
to get right but yields far fewer separately explainable layers.

**What we accept.** Reads must consult several sources — the active memtable, immutable
memtables, and one or more SSTables per level — so a point lookup costs more than a B+tree's
single root-to-leaf descent. This is the read amplification the Bloom filters and block cache
exist to bound. Space amplification from dead versions is real until compaction runs. Write
amplification is higher than a B+tree's for update-heavy workloads and is a number this
project is obligated to measure rather than hide.

**What we gain.** Writes become sequential appends rather than random page writes, which is
the property that makes the write path fast on both spinning disks and SSDs, and which makes
the durability boundary a single `fdatasync` on one append-only file rather than a
crash-consistency argument spanning many pages.

**Revisit if** the workload becomes read-dominated with in-place updates and tight p99
requirements, at which point a B+tree or a hybrid becomes the better structure.

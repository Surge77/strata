# MANIFEST and CURRENT

The **MANIFEST** (`MANIFEST-NNNNNN`) records which SSTables make up the database. It is a
log (`docs/format/log.md`) whose records are **version edits**. Replaying every edit from an
empty state yields the current set of files. **CURRENT** names the live MANIFEST.

## Version edit

A sequence of tagged fields. Each field is a varint32 tag followed by its value:

| Tag | Field | Value |
|---|---|---|
| 1 | comparator name | varint-length-prefixed string |
| 2 | log number | varint64: logs numbered below this are obsolete |
| 3 | next file number | varint64 |
| 4 | last sequence | varint64 |
| 5 | deleted file | varint32 level, varint64 file number |
| 6 | new file | varint32 level, varint64 number, varint64 size, prefixed smallest internal key, prefixed largest internal key |

An unknown tag, a truncated field, a level ≥ 7, or a malformed key is `Corruption`. A field may
appear in any order; later values of tags 1–4 replace earlier ones.

## Publishing a change

Every change to the set of files (flush, compaction) follows the same order. Each step is safe
to crash after:

1. Write and `fdatasync` the new SSTable, then `SyncDir`, so the file's name is durable.
2. Append the edit to the MANIFEST and `fdatasync` it. **The change is committed here.**
3. Only then delete files the edit made obsolete (old logs, replaced tables).

A new MANIFEST is started at `Open`. It is written with a snapshot of the whole current state,
synced, then published by atomically replacing CURRENT:

1. write `MANIFEST-NNNNNN\n` to `NNNNNN.dbtmp`, `fdatasync` it;
2. `rename` it over `CURRENT`;
3. `SyncDir`.

A crash anywhere leaves CURRENT naming either the old, complete MANIFEST or the new, complete
one. It never names a partial one.

## Recovery

`Open` reads CURRENT, replays the MANIFEST it names, and verifies the result. Missing CURRENT
with no other database files means a new database. CURRENT pointing at a missing or corrupt
MANIFEST is `Corruption`. A torn final record in the MANIFEST is discarded, like any torn log
tail: the change it described was never committed. Then the write-ahead logs numbered at or
above the recovered log number are replayed, flushed to a new level-0 table, and committed
through the protocol above.

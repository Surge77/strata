# SSTable format

An SSTable ("sorted string table", `NNNNNN.sst`) is an immutable file of key/value pairs sorted
by key. The memtable is flushed into one (phase 5), and compaction (phase 7) merges them. Once
written, an SSTable is never modified. That is what lets any number of readers share it without
locks, and what makes a checksum verified once stay meaningful.

Keys stored in an SSTable are **internal keys** (`docs/format/internal_key.md`), compared with
the internal key comparator.

## File layout

```
+---------------------+
| data block 0        |  each block is followed by its 5-byte trailer
| data block 1        |
| ...                 |
| data block N-1      |
| meta blocks         |  optional whole-table Bloom filter from phase 6
| metaindex block     |  maps meta block names to their handles
| index block         |  one entry per data block
| footer (48 bytes)   |  fixed size, at the very end of the file
+---------------------+
```

A reader opens a table from the end: read the footer, which locates the index and metaindex
blocks. The index then locates any data block.

The optional Bloom filter is specified in [bloom_filter.md](bloom_filter.md).

## Block handle

A pointer to a block:

```
varint64 offset   -- file offset of the block's first byte
varint64 size     -- length of the block contents, excluding the 5-byte trailer
```

At most 20 bytes (`kMaxEncodedLength`).

## Block trailer

Every block, of every kind, is followed by:

```
uint8    compression type  -- 0 = none (phase 14 adds 1 = snappy, 2 = zstd)
fixed32  checksum          -- crc32c::Mask(crc32c(block contents ‖ compression type))
```

The checksum covers the contents and the type byte, so a damaged type is caught. Every block is
verified every time it is read from the file. A block that fails is reported as `Corruption`,
never interpreted.

## Block contents

Data, index, and metaindex blocks share one format: a sequence of sorted entries with **prefix
compression**, followed by a **restart array**.

```
entry 0
entry 1
...
entry K-1
fixed32 restart[0]    -- offset of an entry whose key is stored in full
...
fixed32 restart[R-1]
fixed32 R             -- number of restart points, R >= 1
```

Each entry:

```
varint32 shared        -- bytes shared with the previous entry's key
varint32 non_shared    -- bytes of this key after the shared prefix
varint32 value_length
bytes    key_delta     -- non_shared bytes
bytes    value         -- value_length bytes
```

Every `block_restart_interval` entries (16 by default), prefix compression restarts: the entry
has `shared = 0` and its offset is recorded in the restart array. A seek binary-searches the
restart points, whose keys are complete, then scans forward at most one interval. Adjacent keys
in sorted order share long prefixes, so most entries store only a few key bytes.

A block reader must check every length against the bytes present: `shared` must not exceed the
previous key, the restart array must fit, and every restart offset must lie before the array.
Any violation is `Corruption`.

## Index block

One entry per data block. The **key** is a *separator*: a key ≥ every key in that block and
< every key in the next. The builder picks the shortest such key (e.g. between `"the quick"` and
`"the whale"` it uses `"the r"`), which keeps the index small. The last block's separator is
the shortest key ≥ its last key. The **value** is the block's handle.

## Footer

```
metaindex handle   \
index handle        } padded with zeros to 40 bytes
fixed64 magic      -- 0x5354524154415354  ("STRATAST")
```

48 bytes, always the last bytes of the file. A file shorter than 48 bytes, or with the wrong magic
number, is not an SSTable (`Corruption`).

## Limits and invariants

- Keys within a table are strictly increasing under the internal key comparator. The builder
  refuses out-of-order keys.
- The target data block size is 4 KiB (a block is cut after the first entry that crosses it),
  so a block is read from disk in one or two pages.
- Every length and offset read from the file is untrusted, as in the log format.

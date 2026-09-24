# Internal key format

Users see keys. The engine stores **internal keys**: the user key plus the version
information that lets many versions of one key coexist, one per write.

```
+--------------------+---------------------------------------+
| user key           | trailer                               |
| any bytes          | fixed64( sequence << 8 | type )       |
+--------------------+---------------------------------------+
                       8 bytes, little-endian
```

- **sequence** is a 56-bit number, unique per operation and increasing with every write. The
  write path assigns consecutive sequence numbers to the operations of a batch, in order.
- **type** is `0x00` (`kDeletion`, a tombstone) or `0x01` (`kValue`). Any other value is
  `Corruption` when read from disk.

## Ordering

Internal keys sort by:

1. **user key, ascending** (bytewise, unsigned: `memcmp` order);
2. **sequence, descending**;
3. **type, descending**.

Rules 2 and 3 together are "trailer descending". The newest version of a key therefore sorts
*first*. A reader looking for "the value of `k` as of sequence `s`" seeks to the internal key
`(k, s, kValue)` and takes the first entry at or after it. If that entry has user key `k`, it is
the newest version visible at `s`. Older versions stay in place behind it. That is what snapshots
(phase 7) read.

The seek key uses `kValue`, the highest type, so it sorts before every real entry `(k, s, *)`:
a seek at sequence `s` finds a write made *at* `s`, not only writes made before it.

## Memtable entry encoding

In the memtable each entry is one arena allocation:

```
varint32  internal key length  (user key length + 8)
bytes     internal key
varint32  value length          (0 for a tombstone)
bytes     value
```

## Limits

A user key or value can be at most `2^32 − 1` bytes, because every length in the engine's formats
is a varint32. The DB rejects anything larger with `InvalidArgument` rather than truncating it.

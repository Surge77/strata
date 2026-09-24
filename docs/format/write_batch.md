# WriteBatch format

A `WriteBatch` is an ordered list of `Put` and `Delete` operations applied atomically. Its
serialised form is exactly the payload of one WAL record, so "the batch is in the log" and
"every operation in the batch is in the log" are the same statement. That is what makes a
multi-key write atomic across a crash.

## Layout

```
+-----------+---------+------------------------------------+
| sequence  | count   | operations                         |
| fixed64   | fixed32 | count × operation                  |
+-----------+---------+------------------------------------+
  8 bytes     4 bytes   (12-byte header, kHeaderSize = 12)

operation :=  kTypeValue    (1 byte = 0x01)  varstring key  varstring value
           |  kTypeDeletion (1 byte = 0x00)  varstring key

varstring :=  varint32 length  bytes
```

- `sequence` is the sequence number of the **first** operation. Operation *i* (0-based) gets
  `sequence + i`. The writer assigns it just before appending the batch to the WAL. A batch
  that has not been written yet carries 0.
- `count` is the number of operations. A decoder must find exactly `count` operations and then
  the end of the buffer. Extra bytes, missing operations, or an unknown type byte are
  `Corruption`.
- Keys and values are arbitrary bytes, including NULs. Each is limited to `2^32 - 1` bytes by
  its varint32 length.

The type byte values are shared with internal keys (phase 3). There they are packed into the
low 8 bits of the key's trailer, below the sequence number. A deletion is written as a
*tombstone*, a record saying the key is gone, and is not removed from anything in place.
Removal happens only when compaction (phase 7) proves no older version can reappear.

## Why a deletion carries no value

A tombstone only has to say "this key is gone as of sequence N". Storing an empty value would
cost one byte per delete and make "a Put of the empty string" ambiguous with "a Delete".

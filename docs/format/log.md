# Log file format

Used by the write-ahead log (`NNNNNN.log`) and, from phase 5, by the MANIFEST. The log layer
knows nothing about what it carries: it turns a sequence of byte strings ("records") into a
file, and back.

## Goals

1. **A torn write at the end is not corruption.** A process killed mid-append leaves a partial
   record. The reader treats it as end-of-log, because nothing after it was ever acknowledged
   as durable.
2. **Corruption in the middle is contained.** A damaged record is reported and skipped, and
   reading resumes at the next intact record: one bad sector does not cost the rest of the log.
3. **Every byte is checksummed** except padding, so no silent corruption reaches the memtable.

## Blocks

The file is a sequence of **32 KiB blocks** (`kBlockSize = 32768`). The last block may be
partial. Blocks exist for resynchronisation: after corruption the reader can always restart at
the next multiple of 32 KiB, where a record header must begin.

## Fragments

A record is written as one or more **fragments**, each with a 7-byte header:

```
+-----------+-----------+--------+----------------+
| checksum  | length    | type   | payload        |
| fixed32   | fixed16   | uint8  | length bytes   |
+-----------+-----------+--------+----------------+
  4 bytes     2 bytes     1 byte
```

- All integers are little-endian (see `util/coding.hpp`).
- `checksum` = `crc32c::Mask(crc32c(type byte ‖ payload))`. The type byte is covered, so a
  flipped type is detected. The value is masked because CRCs of data that itself contains CRCs
  are otherwise correlated (see `crc32c.hpp`).
- `length` = payload bytes in this fragment, 0 to `kBlockSize - 7`.
- `type`:

| Value | Name | Meaning |
|---|---|---|
| 0 | `kZero` | reserved: never written. A zero type means the reader hit zero-filled space (pre-allocation or padding) |
| 1 | `kFull` | the entire record is in this fragment |
| 2 | `kFirst` | first fragment of a multi-fragment record |
| 3 | `kMiddle` | an interior fragment |
| 4 | `kLast` | the final fragment |

A fragment never crosses a block boundary. When a record does not fit in the rest of the
current block, it is split: `kFirst` fills the block, `kMiddle` fragments fill whole blocks,
and `kLast` carries the remainder.

## Trailers

If fewer than 7 bytes remain in a block, no header fits. The writer fills those bytes with
zeros and starts the next fragment in the next block. The reader skips any block remainder
shorter than a header. An empty record (length 0) is legal and is written as a single `kFull`
fragment with no payload.

## Reader contract

The reader returns complete records in order. For each problem it meets it either stops or
reports and skips:

| Situation | Treatment |
|---|---|
| End of file exactly at a record boundary | end of log |
| End of file inside a header or payload (torn tail) | end of log; **not** reported as corruption |
| Checksum mismatch | report the rest of the block dropped; resume at the next block. The length field is unverified, so following it could land mid-payload |
| `length` runs past the end of the block | report the rest of the block dropped; skip to the next block |
| `type` is `kZero` with zero length | skip the rest of the block silently (zero-filled space) |
| Unknown type | report the fragment dropped |
| `kMiddle`/`kLast` without a preceding `kFirst` | report the fragment dropped |
| `kFirst` or `kFull` while a record is in progress | report the partial record dropped; start the new one |

Reports go to a caller-supplied `Reporter` (`Corruption(bytes_dropped, Status)`). Recovery
(phase 3) decides whether a report is fatal. The default is to fail open, so no acknowledged
write is silently discarded.

## Worked example

A 100-byte record, then a 40,000-byte record, starting at offset 0:

```
offset 0      header(kFull, 100)            7 bytes
offset 7      payload                     100 bytes
offset 107    header(kFirst, 32654)         7 bytes     block 0 ends at 32768
offset 114    payload                   32654 bytes
offset 32768  header(kLast, 7346)           7 bytes     block 1
offset 32775  payload                    7346 bytes
```

(32654 = 32768 − 107 − 7. The first record used 107 bytes and the header takes 7 more.)

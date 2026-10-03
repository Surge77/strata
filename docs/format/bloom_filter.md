# Table Bloom filter format

Phase 6 tables may contain one whole-table Bloom filter. The filter answers
whether a user key might appear anywhere in the table. A negative answer is
definitive; a positive answer still requires a normal index and data-block
lookup. Older tables have no filter and remain readable.

The metaindex contains an entry whose bytewise key is
`filter.strata.user-bloom.v1`. Its value is a block handle. That handle points
to a raw filter block, protected by the same compression byte and CRC-32C
trailer as every other SSTable block. The filter is written before the
metaindex block. Unknown metaindex entries are ignored.

Filter contents are one byte `k = 7`, followed by at least eight bytes of
bitset. The bitset length is always a multiple of eight bytes. A writer uses
ten bits per inserted key, rounded up to a multiple of 64 bits, with a minimum
of 64 bits. Duplicate user keys may be inserted more than once. All bits start
at zero. The hash is 64-bit FNV-1a over the user key bytes (offset basis
`14695981039346656037`, prime `1099511628211`), followed by the SplitMix64
finalizer (`xor >> 30`, multiply by `0xBF58476D1CE4E5B9`, `xor >> 27`,
multiply by `0x94D049BB133111EB`, `xor >> 31`). Let `h1` be its low 32 bits and `h2` its high 32 bits
with the low bit set. For `i = 0..6`, set or test bit `(h1 + i * h2) % bit_count`.
Multi-byte arithmetic is unsigned and wraps modulo 2^32 where applicable.

Only the user-key portion of each stored internal key is hashed. Sequence
numbers and value/deletion types are excluded, so every version of one user
key answers the same filter query. An invalid filter length or `k` is
`Corruption` when the table opens. A missing filter falls back to the normal
read path; it never causes a negative answer.

This v1 filter is used only with the bytewise user comparator. A comparator
that equates different byte strings needs its own canonical key encoding;
readers using such a comparator ignore a v1 filter, and writers reject the
filtered-table option. `Table::Seek` still returns the first key at or after
its target. Only an exact user-key lookup may skip a table after a negative
filter result.

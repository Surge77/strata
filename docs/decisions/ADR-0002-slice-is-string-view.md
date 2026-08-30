# ADR-0002: `Slice` is an alias for `std::string_view`

**Status:** accepted · **Date:** 2026-08-30

## Context

The engine passes non-owning byte ranges everywhere: keys and values across the public API,
records inside a WAL buffer, entries inside an SSTable data block. LevelDB and RocksDB both
define a bespoke `Slice` class for this. Copying that design is the obvious default.

`Slice` in those codebases exists because they predate C++17. It is a pointer, a length, a
lexicographic `compare`, and a few conveniences — all of which `std::string_view` now provides.

Two properties are non-negotiable for this engine:

1. **Arbitrary bytes, including embedded NULs.** Length must be explicit, never derived from
   a terminator.
2. **Unsigned lexicographic ordering.** `char` is signed on x86-64. If comparison used signed
   `char`, the key `"\x80"` would sort *before* `"\x01"`, silently corrupting the ordering of
   every non-ASCII key in every SSTable.

## Decision

`using Slice = std::string_view;`, plus three free functions (`AsSlice`, `AsBytes`,
`SharedPrefixLength`) in `include/strata/slice.hpp`.

## Consequences

`std::string_view` satisfies both requirements. Its size is explicit, so NULs are ordinary
bytes. Its comparison goes through `std::char_traits<char>::compare`, which the standard
specifies to behave like `memcmp` — that is, unsigned — regardless of the signedness of `char`
on the platform. `tests/unit/slice_test.cpp` pins this behaviour so a future change of vocabulary
type cannot quietly break it.

**What we gain.** Roughly 150 lines of code we do not write, test, or maintain. Every standard
algorithm, `std::format`, and every third-party API already speaks `string_view`, so no adapter
layer is needed at any boundary. Readers do not have to learn a project-specific type to read the
signatures.

**What we accept.** The name `Slice` cannot carry engine-specific methods, so shared-prefix
computation lives as a free function instead of a member. The type name in a compiler diagnostic
is `std::basic_string_view<char>` rather than `strata::Slice`. Neither costs anything real.

**Why not `std::span<const std::byte>`.** It is the honest type for "some bytes", but it has no
lexicographic comparison, no hashing, and no literal syntax, and every key comparison would need
a hand-written `memcmp` wrapper. `AsSlice`/`AsBytes` convert at the two boundaries where byte
spans genuinely appear, which confines `reinterpret_cast` to one header.

**Revisit if** the engine needs a slice that owns or refcounts its buffer — that is a different
type (`PinnableSlice` in RocksDB), not a reason to hand-roll this one.

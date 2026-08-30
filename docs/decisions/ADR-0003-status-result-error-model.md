# ADR-0003: Errors are returned as `Status` / `Result<T>`, never thrown

**Status:** accepted · **Date:** 2026-08-30

## Context

A storage engine fails in ordinary, expected ways: a key is absent, a block fails its checksum,
a write returns `ENOSPC`, another process holds the database lock. These are not exceptional
conditions — they are outcomes the caller must handle. The source specification left the choice
open ("use exceptions only when justified"), which is exactly the ambiguity that produces a
codebase where half the call sites check and half do not.

## Decision

Every fallible operation returns `Status`, or `Result<T>` when it also produces a value. The
engine never throws across an API boundary. Exceptions remain enabled — `std::string` and
`std::vector` may still throw `std::bad_alloc`, and disabling them would fight the standard
library for no benefit — but no engine code throws deliberately, and no engine API documents a
throwing contract. Tools and tests may use exceptions freely.

## Consequences

**Why not exceptions.** `NotFound` is the single most common result of `Get` on a real workload.
Making the common case a thrown exception is both slow and semantically wrong. Error paths in a
storage engine are also frequently *recoverable in a specific way* — a corrupt WAL tail is
truncated, a corrupt SSTable block is reported — and the handling belongs next to the call, not
in a distant `catch`.

**Making it hard to ignore.** `Status` is declared `[[nodiscard]]`, as is `Result<T>`, so
dropping one is a compiler error rather than a silent bug. `STRATA_RETURN_IF_ERROR` and
`STRATA_ASSIGN_OR_RETURN` keep propagation to one line so that checking is never the verbose
option.

**Cost.** `Status` is 16 bytes: a code plus a pointer to an optional message. The success path
allocates nothing, because the message pointer stays null; only a failure with a message reaches
the allocator. Failure is the cold path, so this is the right side of the trade.

**Why not `std::expected`.** It is the same design, standardised — but it is C++23, and this
project's baseline is C++20. `Result<T>` is deliberately a subset of its interface (`ok()`,
`value()`, `operator*`, `operator->`, `value_or`), so adopting `std::expected` later is a
mechanical substitution rather than a redesign.

**Revisit if** the toolchain baseline moves to C++23, at which point `Result<T>` should become an
alias for `std::expected<T, Status>`.

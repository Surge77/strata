# Fix: the clang build

**Branch:** `fix/clang-build`, off `main` after phase 11 and the rotation fix.

## Goal

CI's first push of `main` in seven weeks (2026-11-13) was red: every clang job (clang
Debug and RelWithDebInfo, ASan, TSan, libFuzzer) failed to compile. GCC jobs and
coverage passed. Make `main` build with clang again, and close the gap that let a
clang-only error sit unnoticed since phase 3.

## Roles and methods

- **Senior C++ engineer:** which rule does clang enforce that GCC does not?
- **Principal engineer (five whys):** why did seven weeks of phases never hit it?

## Cause

```text
src/db/skiplist.hpp:132:24: error: no type named 'Node' in 'SkipList<Key, Comparator>'
```

`SkipList::Iterator` declared its data member as `typename SkipList::Node* node_`, but
`Node` is defined later, in the outer class's private section. Member function bodies
are a complete-class context, so the uses of `Node` inside them were fine. A nested
class's member *declarations* are not, so at that point `Node` does not exist yet. Clang
rejects that. GCC accepts it.

Five whys: CI was red → clang cannot compile `skiplist.hpp` → the iterator names a
type declared further down → GCC accepted it, and the only local compiler is GCC
13 → no clang in WSL and no sudo to install one, and phases were merged locally and
pushed in one batch, so CI never saw a single phase's change.

## Fix

Forward-declare `struct Node;` at the top of the class, before `Iterator`, and declare
the member as plain `Node*`. No behaviour change.

## Verification

- A local clang is now available without root: the official LLVM 18.1.8 release,
  unpacked into `~/.local/llvm-18` (driver, clang-tidy, lld, and the compiler-rt
  sanitizer and libFuzzer runtimes). Its `libtinfo.so.5` dependency comes from Ubuntu's
  `libtinfo5` package, unpacked with `dpkg-deb -x` into `~/.local/llvm-18/compat` and
  found through `LD_LIBRARY_PATH`.
- clang 18 with ASan/UBSan and the libFuzzer targets (CI's harshest clang job):
  builds with no errors or warnings under `-Werror`, and passes **438/438**.
- clang-format check is clean.

## What went wrong

The clang jobs existed in CI from phase 0, but a CI matrix only protects what is
pushed. From here on, every phase also builds and tests with the local clang before it
merges.

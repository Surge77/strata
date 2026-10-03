#!/usr/bin/env bash
# End-to-end test of the stratactl data commands, run by ctest.
#
# Each command is a separate process, so this also checks that data written by
# one process is recovered from the WAL by the next.
set -euo pipefail

STRATACTL="$1"
DB="$(mktemp -d "${STRATA_TEST_DIR:-/tmp}/stratactl-XXXXXX")/db"
trap 'rm -rf "$(dirname "$DB")"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

"$STRATACTL" put "$DB" user:101 rahul
[[ "$("$STRATACTL" get "$DB" user:101)" == "rahul" ]] || fail "get after put"

"$STRATACTL" put "$DB" user:101 priya --sync
[[ "$("$STRATACTL" get "$DB" user:101)" == "priya" ]] || fail "overwrite"

"$STRATACTL" delete "$DB" user:101
set +e
"$STRATACTL" get "$DB" user:101 2>/dev/null
code=$?
set -e
[[ $code -eq 1 ]] || fail "get of a deleted key should exit 1, got $code"

set +e
"$STRATACTL" get "$DB" 2>/dev/null
code=$?
set -e
[[ $code -eq 2 ]] || fail "missing argument should exit 2, got $code"

[[ -n "$("$STRATACTL" version)" ]] || fail "version"

# Opening for get flushes the replayed log into a real level-0 SSTable.
DUMP_DB="$(dirname "$DB")/dump-db"
"$STRATACTL" put "$DUMP_DB" dump:key dump-value
[[ "$("$STRATACTL" get "$DUMP_DB" dump:key)" == "dump-value" ]] || fail "reopen before dump"
sst_files=("$DUMP_DB"/*.sst)
[[ ${#sst_files[@]} -gt 0 && -f ${sst_files[0]} ]] || fail "recovery did not create an SSTable"
dump="$("$STRATACTL" dump-sst "${sst_files[0]}")" || fail "dump-sst failed"
[[ "$dump" == *"'dump:key'"*"PUT => 'dump-value'"* ]] || fail "dump-sst missed the key"
[[ "$dump" == *" entries" ]] || fail "dump-sst missed its entry count"

echo "stratactl: all checks passed"

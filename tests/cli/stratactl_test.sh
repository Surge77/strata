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

# stats: valid JSON for an existing database; asking never creates one.
stats="$("$STRATACTL" stats "$DUMP_DB")" || fail "stats failed"
[[ "$stats" == '{"unix_micros":'* ]] || fail "stats is not the stats object"
if command -v python3 >/dev/null; then
  python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["levels"][0]["files"]>=1' \
      <<<"$stats" || fail "stats is not valid JSON with a level-0 table"
fi
set +e
"$STRATACTL" stats "$(dirname "$DB")/nothing-here" 2>/dev/null
code=$?
set -e
[[ $code -eq 3 ]] || fail "stats of a missing database should exit 3, got $code"
[[ ! -e "$(dirname "$DB")/nothing-here" ]] || fail "stats created a database"

# serve: answers over HTTP, then shuts down cleanly on SIGTERM.
OUT="$(dirname "$DB")/serve.out"
"$STRATACTL" serve "$DUMP_DB" --port 0 >"$OUT" &
pid=$!
for _ in $(seq 50); do
  grep -q '^serving ' "$OUT" 2>/dev/null && break
  sleep 0.1
done
port="$(sed -n 's|^serving http://127.0.0.1:\([0-9]*\)/stats$|\1|p' "$OUT")"
[[ -n "$port" ]] || { kill "$pid"; fail "serve did not report its port"; }
exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
reply="$(cat <&3)"
exec 3<&-
[[ "$reply" == "HTTP/1.1 200 OK"* && "$reply" == *"ok" ]] || { kill "$pid"; fail "healthz: $reply"; }
kill -TERM "$pid"
wait "$pid" || fail "serve did not exit cleanly on SIGTERM"
set +e
"$STRATACTL" serve "$DUMP_DB" --port notaport 2>/dev/null
code=$?
set -e
[[ $code -eq 2 ]] || fail "a bad port should exit 2, got $code"

echo "stratactl: all checks passed"

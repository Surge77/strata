#!/usr/bin/env bash
# End-to-end test of stratasoak, run by ctest: a short mixed workload that
# serves /stats and the dashboard while it runs, then shuts down cleanly.
set -euo pipefail

SOAK="$1"
DIR="$(mktemp -d "${STRATA_TEST_DIR:-/tmp}/stratasoak-XXXXXX")"
trap 'rm -rf "$DIR"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

OUT="$DIR/soak.out"
"$SOAK" --db="$DIR/db" --duration_s=4 --progress_s=1 --serve_port=0 --keys=2000 \
    --writers=2 --readers=2 >"$OUT" 2>"$DIR/soak.err" &
pid=$!
for _ in $(seq 50); do
  grep -q '^dashboard ' "$OUT" 2>/dev/null && break
  sleep 0.1
done
port="$(sed -n 's|^dashboard http://127.0.0.1:\([0-9]*\)/$|\1|p' "$OUT")"
[[ -n "$port" ]] || { kill "$pid"; fail "no dashboard line: $(cat "$OUT" "$DIR/soak.err")"; }
sleep 1
exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /stats HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
reply="$(cat <&3)"
exec 3<&-
[[ "$reply" == "HTTP/1.1 200 OK"* && "$reply" == *'"write.calls":'* ]] \
  || { kill "$pid"; fail "stats during the run: ${reply:0:200}"; }
wait "$pid" || fail "stratasoak failed: $(cat "$DIR/soak.err")"
grep -q '^ok$' "$OUT" || fail "no final ok: $(cat "$OUT")"
grep -q '^t=[0-9]*s writes=[1-9]' "$OUT" || fail "no writes reported: $(cat "$OUT")"

# A bad flag is a usage error.
set +e
"$SOAK" --db="$DIR/db" --no_such_flag=1 2>/dev/null
code=$?
set -e
[[ $code -eq 2 ]] || fail "bad flag should exit 2, got $code"

echo "stratasoak CLI: ok"

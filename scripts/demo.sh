#!/usr/bin/env bash
# A five-minute tour of strata, using the binaries of a normal build
# (scripts/build.sh). Each step says what it shows; --no-pause runs straight
# through (as the CLI test does), --dashboard_s N keeps the dashboard up for N
# seconds (default 120).
#
#   1. The basics: put, get, delete, and data surviving each process exit.
#   2. Crash recovery: kill -9 a process in the middle of writing, reopen,
#      and read what recovery did from the engine's own events.
#   3. The dashboard: a mixed workload served live at http://127.0.0.1:<port>/.
set -euo pipefail

BUILD_DIR="${STRATA_BUILD_DIR:-$HOME/build/strata}"
CTL="$BUILD_DIR/tools/stratactl/stratactl"
SOAK="$BUILD_DIR/tools/stratasoak/stratasoak"
PAUSE=1
DASHBOARD_S=120
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-pause)    PAUSE=0 ;;
    --dashboard_s) DASHBOARD_S="$2"; shift ;;
    -h|--help)     sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "demo.sh: unknown option '$1'" >&2; exit 2 ;;
  esac
  shift
done
for binary in "$CTL" "$SOAK"; do
  [[ -x "$binary" ]] || { echo "demo.sh: $binary not found; run scripts/build.sh" >&2; exit 1; }
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/strata-demo-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
run() { printf '$ %s\n' "$*"; "$@"; }
pause() { if [[ $PAUSE == 1 ]]; then read -r -p "(enter to continue) " _; fi; }

step "1. The basics: every command is a separate process"
DB="$WORK/basics"
run "$CTL" put "$DB" user:101 rahul
run "$CTL" put "$DB" user:102 priya --sync
run "$CTL" get "$DB" user:101
run "$CTL" delete "$DB" user:101
run "$CTL" get "$DB" user:101 || echo "(exit $?: NotFound, as it should be)"
run "$CTL" get "$DB" user:102
pause

step "2. Crash recovery: kill -9 in the middle of a write workload"
DB="$WORK/crash/db"
echo "Writing with 2 writers and 2 readers, then killing the process without warning..."
"$SOAK" --db="$DB" --duration_s=60 --progress_s=1 > "$WORK/crash.out" 2>&1 &
pid=$!
sleep 3
kill -9 "$pid"
wait "$pid" 2>/dev/null || true
tail -1 "$WORK/crash.out"
echo
echo "Reopening. Every write the process was told succeeded was in the log; recovery"
echo "replays it. The engine records what it did as events:"
printf '$ %s stats %s\n' "$CTL" "$DB"
"$CTL" stats "$DB" | python3 -c '
import json, sys
stats = json.load(sys.stdin)
for e in stats["recovery_events"]:
    fields = " ".join(f"{k}={v}" for k, v in e["fields"].items())
    level, name = e["level"], e["name"]
    print(f"  {level:5} {name:24} {fields}")
print("  sequence.last =", stats["gauges"]["sequence.last"], "writes applied in total")'
pause

step "3. The dashboard: a live workload at http://127.0.0.1:<port>/"
echo "Open the URL below in a browser (from Windows too, under WSL2). It runs for"
echo "${DASHBOARD_S}s; Ctrl-C ends it early."
run "$SOAK" --db="$WORK/dashboard/db" --duration_s="$DASHBOARD_S" --progress_s=30 \
    --serve_port=0 --keys=300000 --value_size=400

step "Done. Read more"
echo "  docs/durability.md      what survives a crash and what survives power loss"
echo "  docs/observability.md   every metric, event, and the dashboard"
echo "  docs/limitations.md     what strata does not do"

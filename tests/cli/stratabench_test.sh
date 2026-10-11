#!/usr/bin/env bash
# End-to-end smoke test of stratabench, run by ctest: every workload and the
# open-loop harness run on a tiny database, and the JSON report parses.
#
# Rounds this short are noisy, so --allow_unstable keeps the exit status about
# correctness; the stability gate itself is unit tested (bench_stats_test).
set -euo pipefail

STRATABENCH="$1"
SCRATCH="$(mktemp -d "${STRATA_TEST_DIR:-/tmp}/stratabench-XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

"$STRATABENCH" --db="$SCRATCH/db" --num=2000 --min_round_s=0.05 --rounds=1 --warmup=0 \
    --workloads=cpu,fillseq,fillrandom,fillsync,readrandom,readzipf,readmissing,scanfull,seekrandom,ycsb-a,ycsb-b,latency \
    --rates=2000 --duration_s=0.1 --json="$SCRATCH/report.json" --allow_unstable \
    > "$SCRATCH/stdout.txt" 2> "$SCRATCH/stderr.txt" || fail "run exited $?: $(cat "$SCRATCH/stderr.txt")"

grep -q "NOT PUBLISHABLE" "$SCRATCH/stdout.txt" || fail "a 1-round run must be marked not publishable"
grep -q "^ycsb-b" "$SCRATCH/stdout.txt" || fail "missing ycsb-b row"
grep -q "^no-op" "$SCRATCH/stdout.txt" || fail "missing harness floor row"
python3 - "$SCRATCH/report.json" <<'PY' || fail "report.json is wrong"
import json, sys
report = json.load(open(sys.argv[1]))
assert report["publishable"] is False
assert len(report["workloads"]) == 11, len(report["workloads"])
assert all(w["summary"]["rounds"] == 1 and w["summary"]["median"] > 0 for w in report["workloads"])
assert report["latency"]["rows"][0]["offered_rate"] == 2000
assert report["env"]["filesystem"] != "9p"
PY
[[ ! -e "$SCRATCH/db/loaded" && ! -e "$SCRATCH/db/fill" ]] || fail "databases left behind"

set +e
"$STRATABENCH" --db="$SCRATCH/db" --workloads=nosuch 2>/dev/null
code=$?
"$STRATABENCH" --workloads=fillseq 2>/dev/null
code_missing_db=$?
"$STRATABENCH" --db="$SCRATCH/db" --min_round_s=nan 2>/dev/null
code_nan=$?
"$STRATABENCH" --db="$SCRATCH/db" --workloads=fillseq --num=100 --rounds=1 --warmup=0 >/dev/null 2>&1
code_one_round=$?
set -e
[[ $code -eq 2 ]] || fail "unknown workload should exit 2, got $code"
[[ $code_missing_db -eq 2 ]] || fail "missing --db should exit 2, got $code_missing_db"
[[ $code_nan -eq 2 ]] || fail "--min_round_s=nan should exit 2, got $code_nan"
[[ $code_one_round -eq 3 ]] || fail "an unpublishable run should exit 3, got $code_one_round"
echo "stratabench smoke test passed"

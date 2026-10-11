#!/usr/bin/env bash
# Run stratabench and keep everything a result needs to be trusted later.
#
# Each run gets a fresh directory under $STRATA_BENCH_RESULTS (default
# ~/strata-bench/), named <UTC time>-<commit>, holding report.json, stdout.txt,
# and with --profile the perf data and reports. Databases are created under
# $STRATA_BENCH_DIR, which, like the build tree, must be on ext4 and never on
# /mnt/c -- stratabench refuses a 9P scratch directory itself.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${STRATA_BUILD_DIR:-$HOME/build/strata}"
SCRATCH="${STRATA_BENCH_DIR:-$HOME/.cache/strata-bench-data}"
RESULTS_ROOT="${STRATA_BENCH_RESULTS:-$HOME/strata-bench}"
PERF="${PERF:-perf}"
# perf samples at this rate (Hz); 999 rather than 1000 avoids lockstep with
# periodic timers.
PERF_FREQUENCY=999
PERF_PERCENT_LIMIT=0.5

usage() {
  cat <<'USAGE'
Usage: scripts/bench.sh [options] [-- <extra stratabench flags>]

  --open-loop            run the open-loop latency harness (--workloads=latency)
  --profile <workload>   one round of <workload> under perf, sampling only the
                         workload itself (not the load that precedes it)
  --cpus <list>          pin the run with taskset, e.g. --cpus 0-3
  --build-dir <path>     override the build directory
  -h, --help             show this message

Environment: STRATA_BUILD_DIR, STRATA_BENCH_DIR, STRATA_BENCH_RESULTS, PERF
USAGE
}

MODE=suite
PROFILE_WORKLOAD=""
CPUS=""
EXTRA=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --open-loop) MODE=open-loop ;;
    --profile)   MODE=profile; PROFILE_WORKLOAD="$2"; shift ;;
    --cpus)      CPUS="$2"; shift ;;
    --build-dir) BUILD_DIR="$2"; shift ;;
    -h|--help)   usage; exit 0 ;;
    --)          shift; EXTRA=("$@"); break ;;
    *) echo "bench.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

STRATABENCH="$BUILD_DIR/tools/stratabench/stratabench"
if [[ ! -x "$STRATABENCH" ]]; then
  echo "bench.sh: no stratabench at $STRATABENCH -- run scripts/build.sh first" >&2
  exit 1
fi

COMMIT="$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD)"
if [[ -n "$(git -C "$REPO_ROOT" status --porcelain --untracked-files=no)" ]]; then
  COMMIT="$COMMIT-dirty"
fi
OUT="$RESULTS_ROOT/$(date -u +%Y%m%dT%H%M%SZ)-$COMMIT"
mkdir -p "$OUT" "$SCRATCH"

LAUNCH=()
if [[ -n "$CPUS" ]]; then
  LAUNCH=(taskset -c "$CPUS")
fi

ARGS=(--db="$SCRATCH" --commit="$COMMIT" --json="$OUT/report.json")
case "$MODE" in
  open-loop) ARGS+=(--workloads=latency) ;;
  profile)   ARGS+=(--workloads="$PROFILE_WORKLOAD" --warmup=0 --rounds=1 --allow_unstable) ;;
esac
ARGS+=("${EXTRA[@]}")

echo "==> results in $OUT"
status=0
if [[ "$MODE" == profile ]]; then
  CTL="$OUT/perf.ctl"
  ACK="$OUT/perf.ack"
  mkfifo "$CTL" "$ACK"
  # -D -1: start with sampling off; stratabench enables it around the workload.
  # cpu-clock is a software event: WSL2 exposes no hardware PMU. dwarf call
  # graphs, because the build omits frame pointers.
  "${LAUNCH[@]}" "$PERF" record -e cpu-clock -F "$PERF_FREQUENCY" --call-graph dwarf \
      -D -1 --control "fifo:$CTL,$ACK" -o "$OUT/perf.data" -- \
      "$STRATABENCH" "${ARGS[@]}" --perf_ctl="$CTL" --perf_ack="$ACK" \
      2> "$OUT/stderr.txt" | tee "$OUT/stdout.txt" || status=$?
  rm -f "$CTL" "$ACK"
  "$PERF" report -i "$OUT/perf.data" --stdio --no-children -g none --sort symbol \
      --percent-limit "$PERF_PERCENT_LIMIT" > "$OUT/perf-flat.txt" 2>/dev/null
  "$PERF" report -i "$OUT/perf.data" --stdio --no-children --sort symbol \
      --percent-limit 2 > "$OUT/perf-callgraph.txt" 2>/dev/null
  echo "==> flat profile (top symbols):"
  grep -v '^#' "$OUT/perf-flat.txt" | grep -v '^$' \
      | sed -E 's/[[:space:]]+-[[:space:]]+-[[:space:]]*$//' | head -25
else
  "${LAUNCH[@]}" "$STRATABENCH" "${ARGS[@]}" 2> "$OUT/stderr.txt" \
      | tee "$OUT/stdout.txt" || status=$?
fi
echo "==> results in $OUT (exit $status)"
exit "$status"

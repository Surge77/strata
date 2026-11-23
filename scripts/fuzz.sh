#!/usr/bin/env bash
# Run every fuzz target under libFuzzer for a fixed time, in parallel, from a
# corpus seeded with each harness's well-formed inputs (release gate: one hour
# each, reported with the duration that actually ran -- ADR-0004).
#
# Needs two builds of the same tree:
#   scripts/build.sh                                   # GCC driver: writes seeds
#   CC=clang CXX=clang++ scripts/build.sh --asan --libfuzzer \
#       --build-dir ~/build/strata-clang-asan          # the fuzzers themselves
#
# Usage: scripts/fuzz.sh [--seconds N] [--out DIR]
set -uo pipefail

SECONDS_EACH=3600
OUT="${STRATA_FUZZ_OUT:-$HOME/strata-fuzz}"
DRIVER_BUILD="${STRATA_BUILD_DIR:-$HOME/build/strata}"
FUZZ_BUILD="${STRATA_FUZZ_BUILD_DIR:-$HOME/build/strata-clang-asan}"
RSS_LIMIT_MB=1536

while [[ $# -gt 0 ]]; do
  case "$1" in
    --seconds) SECONDS_EACH="$2"; shift ;;
    --out)     OUT="$2"; shift ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "fuzz.sh: unknown option '$1'" >&2; exit 2 ;;
  esac
  shift
done

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
mkdir -p "$OUT"
targets=()
for fuzzer in "$FUZZ_BUILD"/tests/fuzz/*_fuzz; do
  name="$(basename "$fuzzer")"
  targets+=("$name")
  mkdir -p "$OUT/$name/corpus" "$OUT/$name/crashes"
  "$DRIVER_BUILD/tests/fuzz/$name" --write_seeds "$OUT/$name/corpus" > /dev/null || exit 2
  echo "==> $name for ${SECONDS_EACH}s"
  "$fuzzer" -max_total_time="$SECONDS_EACH" -rss_limit_mb="$RSS_LIMIT_MB" \
      -print_final_stats=1 -artifact_prefix="$OUT/$name/crashes/" "$OUT/$name/corpus" \
      > "$OUT/$name/log.txt" 2>&1 &
  echo $! > "$OUT/$name/pid"
done
failed=0
for name in "${targets[@]}"; do
  if wait "$(cat "$OUT/$name/pid")"; then
    runs="$(grep -m1 -oE 'stat::number_of_executed_units: *[0-9]+' "$OUT/$name/log.txt" | grep -oE '[0-9]+$')"
    cov="$(grep -oE 'cov: [0-9]+' "$OUT/$name/log.txt" | tail -1)"
    echo "PASS  $name  ${runs:-?} runs, ${cov:-cov ?}, corpus $(ls "$OUT/$name/corpus" | wc -l)"
  else
    echo "FAIL  $name  (see $OUT/$name/log.txt and $OUT/$name/crashes/)"
    failed=1
  fi
done
exit "$failed"

#!/usr/bin/env bash
# Run the strata test suite.
#
# Test databases are created under STRATA_TEST_DIR, which defaults to a path on
# ext4 for the same fsync-correctness reason described in build.sh.
set -euo pipefail

BUILD_DIR="${STRATA_BUILD_DIR:-$HOME/build/strata}"
TEST_DIR="${STRATA_TEST_DIR:-$HOME/.cache/strata-tests}"
JOBS="${STRATA_JOBS:-$(nproc)}"
LABEL=""

usage() {
  cat <<'USAGE'
Usage: scripts/test.sh [options] [-- <extra ctest args>]

  --unit | --integration | --crash | --model   run only that suite
  --build-dir <path>                           override the build directory
  -h, --help                                   show this message
USAGE
}

CTEST_EXTRA=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --unit)        LABEL=unit ;;
    --integration) LABEL=integration ;;
    --crash)       LABEL=crash ;;
    --model)       LABEL=model ;;
    --build-dir)   BUILD_DIR="$2"; shift ;;
    -h|--help)     usage; exit 0 ;;
    --)            shift; CTEST_EXTRA=("$@"); break ;;
    *) echo "test.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

if [[ ! -d "$BUILD_DIR" ]]; then
  echo "test.sh: no build at $BUILD_DIR -- run scripts/build.sh first" >&2
  exit 1
fi

mkdir -p "$TEST_DIR"
export STRATA_TEST_DIR="$TEST_DIR"

ARGS=(--test-dir "$BUILD_DIR" --output-on-failure -j "$JOBS")
if [[ -n "$LABEL" ]]; then
  ARGS+=(-L "$LABEL")
fi

echo "==> ctest ${LABEL:-all} (data: $TEST_DIR)"
ctest "${ARGS[@]}" "${CTEST_EXTRA[@]}"

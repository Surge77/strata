#!/usr/bin/env bash
# Configure and build strata.
#
# The build tree deliberately defaults to $HOME (ext4) rather than a directory
# inside the repo: on WSL the repo lives under /mnt/c, whose 9P filesystem is
# slow and does not give the fsync semantics this project's tests depend on.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${STRATA_BUILD_DIR:-$HOME/build/strata}"
BUILD_TYPE="${STRATA_BUILD_TYPE:-RelWithDebInfo}"
SANITIZER="${STRATA_SANITIZER:-none}"
COVERAGE="${STRATA_COVERAGE:-OFF}"
JOBS="${STRATA_JOBS:-$(nproc)}"

usage() {
  cat <<'USAGE'
Usage: scripts/build.sh [options]

  --debug              CMAKE_BUILD_TYPE=Debug
  --release            CMAKE_BUILD_TYPE=Release
  --asan               -fsanitize=address,undefined
  --tsan               -fsanitize=thread
  --coverage           instrument for coverage
  --clang              build with clang++ instead of the default compiler
  --clean              delete the build directory first
  --build-dir <path>   override the build directory
  -h, --help           show this message

Environment: STRATA_BUILD_DIR, STRATA_BUILD_TYPE, STRATA_SANITIZER,
             STRATA_COVERAGE, STRATA_JOBS
USAGE
}

CLEAN=0
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug)     BUILD_TYPE=Debug ;;
    --release)   BUILD_TYPE=Release ;;
    --asan)      SANITIZER="address+undefined"; BUILD_TYPE=Debug ;;
    --tsan)      SANITIZER="thread";            BUILD_TYPE=Debug ;;
    --coverage)  COVERAGE=ON; BUILD_TYPE=Debug ;;
    --clang)     EXTRA_ARGS+=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++) ;;
    --clean)     CLEAN=1 ;;
    --build-dir) BUILD_DIR="$2"; shift ;;
    -h|--help)   usage; exit 0 ;;
    *) echo "build.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

if [[ "$CLEAN" == 1 ]]; then
  echo "==> removing $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

GENERATOR="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
  GENERATOR="Ninja"
fi

echo "==> configure  type=$BUILD_TYPE sanitizer=$SANITIZER coverage=$COVERAGE"
cmake -G "$GENERATOR" -S "$REPO_ROOT" -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DSTRATA_SANITIZER="$SANITIZER" \
      -DSTRATA_COVERAGE="$COVERAGE" \
      "${EXTRA_ARGS[@]}"

echo "==> build      jobs=$JOBS"
cmake --build "$BUILD_DIR" -j "$JOBS"

echo "==> built into $BUILD_DIR"

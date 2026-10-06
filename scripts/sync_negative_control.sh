#!/usr/bin/env bash
# Negative control for the crash tests (roadmap release gate): removing any
# single fsync or directory sync from the engine must make a crash test fail.
# If one can be removed and every crash test still passes, either that sync is
# unnecessary or the crash tests are not testing durability.
#
# For each Sync()/SyncDir() call in src/ (the Env implementation excluded), the
# call is replaced by Status::Ok() -- as if the line were deleted -- the tree is
# rebuilt, and the crash suite runs. The file is always restored afterwards.
#
# Usage: scripts/sync_negative_control.sh [--build-dir <path>] [--site <file:line>]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${STRATA_BUILD_DIR:-$HOME/build/strata-negctl}"
JOBS="${STRATA_JOBS:-2}"
ONLY_SITE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift ;;
    --site)      ONLY_SITE="$2"; shift ;;
    -h|--help)   sed -n '2,13p' "$0"; exit 0 ;;
    *)           echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

cd "$ROOT"
export STRATA_TEST_DIR="${STRATA_TEST_DIR:-$HOME/.cache/strata-negctl}"
mkdir -p "$STRATA_TEST_DIR"

# file:line of every call, skipping the Env implementation and comments.
mapfile -t SITES < <(grep -rn -E '(->|\.)(Sync|SyncDir)\(' src --include='*.cpp' \
  | grep -v '^src/util/env_posix.cpp:' \
  | grep -v -E '^[^:]+:[0-9]+:[[:space:]]*//' \
  | cut -d: -f1,2)
if [[ -n "$ONLY_SITE" ]]; then
  SITES=("$ONLY_SITE")
fi
echo "==> ${#SITES[@]} sync call site(s)"

# Warnings stay on but are not fatal: replacing a call can leave a variable
# unused, and a mutant that only trips -Werror says nothing about durability.
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTRATA_WERROR=OFF >/dev/null

# Baseline: the unmodified tree must build and pass the crash suite. A test
# that already fails (or flakes) would otherwise count every site as caught.
echo "==> baseline: unmodified tree"
if ! cmake --build "$BUILD_DIR" -j "$JOBS" >/tmp/negctl-build.log 2>&1; then
  echo "==> ABORT: the unmodified tree does not build (see /tmp/negctl-build.log)"
  exit 2
fi
if ! ctest --test-dir "$BUILD_DIR" -L crash -j "$JOBS" >/tmp/negctl-test.log 2>&1; then
  echo "==> ABORT: the crash suite fails without any change (see /tmp/negctl-test.log)"
  exit 2
fi

BACKUP="$(mktemp)"
CURRENT_FILE=""
restore() {
  if [[ -n "$CURRENT_FILE" ]]; then
    cp "$BACKUP" "$CURRENT_FILE"
    CURRENT_FILE=""
  fi
}
trap 'restore; rm -f "$BACKUP"' EXIT
trap 'exit 130' INT TERM HUP

missed=0
declare -a REPORT
for site in "${SITES[@]}"; do
  file="${site%%:*}"
  line="${site##*:}"
  cp "$file" "$BACKUP"
  CURRENT_FILE="$file"
  # Replace the one call on that line with Status::Ok().
  perl -i -pe 'if ($. == '"$line"') { s/[\w.]+(?:->[\w.]+)*->(?:Sync|SyncDir)\([^()]*\)/Status::Ok()/ }' "$file"
  if cmp -s "$file" "$BACKUP"; then
    restore
    REPORT+=("PATTERN-MISS  $site")
    missed=$((missed + 1))
    continue
  fi
  echo "==> $site: $(sed -n "${line}p" "$BACKUP" | sed 's/^[[:space:]]*//')"
  if ! cmake --build "$BUILD_DIR" -j "$JOBS" >/tmp/negctl-build.log 2>&1; then
    restore
    REPORT+=("BUILD-FAILED  $site")
    missed=$((missed + 1))
    continue
  fi
  if ctest --test-dir "$BUILD_DIR" -L crash --stop-on-failure -j "$JOBS" >/tmp/negctl-test.log 2>&1; then
    REPORT+=("MISSED        $site  (every crash test still passes)")
    missed=$((missed + 1))
  else
    caught_by="$(grep -m1 -oE '[0-9]+ - [A-Za-z0-9_./]+' /tmp/negctl-test.log || echo '?')"
    REPORT+=("caught        $site  by ${caught_by#* - }")
  fi
  restore
done
cmake --build "$BUILD_DIR" -j "$JOBS" >/dev/null 2>&1 || true  # leave a clean build behind

echo
echo "==> results"
printf '%s\n' "${REPORT[@]}"
if [[ $missed -gt 0 ]]; then
  echo "==> FAIL: $missed site(s) not caught"
  exit 1
fi
echo "==> PASS: every sync is load-bearing"

#!/usr/bin/env bash
# Format every tracked C++ source with the repository's .clang-format.
#
# CI rejects unformatted code (clang-format 18, --dry-run --Werror), so run
# this before committing. --check reproduces the CI check without rewriting.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

MODE=(-i)
VERB="formatted"
if [[ "${1:-}" == "--check" ]]; then
  MODE=(--dry-run --Werror)
  VERB="checked"
fi

cd "$REPO_ROOT"
mapfile -t files < <(git ls-files --cached --others --exclude-standard \
                       '*.cpp' '*.hpp' '*.h' '*.cc')
if [[ ${#files[@]} -eq 0 ]]; then
  echo "format.sh: no sources"
  exit 0
fi

"$CLANG_FORMAT" "${MODE[@]}" "${files[@]}"
echo "==> ${#files[@]} files ${VERB}"

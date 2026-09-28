#!/usr/bin/env bash
# Usage: tools/check-format.sh [--fix]
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
fmt=${CLANG_FORMAT:-clang-format-18}
mapfile -t files < <(git ls-files --cached --others --exclude-standard \
    '*.cpp' '*.hpp' '*.h' '*.cc' '*.c' | grep -vE '^(third_party|build)/')
[[ ${#files[@]} -eq 0 ]] && exit 0
if [[ ${1:-} == --fix ]]; then
    "$fmt" -i "${files[@]}"
else
    "$fmt" --dry-run --Werror "${files[@]}"
fi

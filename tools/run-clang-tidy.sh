#!/usr/bin/env bash
# Usage: tools/run-clang-tidy.sh <build-dir> [files...]
# With no files, checks every project translation unit in compile_commands.json.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

build_dir=$1
shift
tidy=${CLANG_TIDY:-clang-tidy-19}

if [[ $# -eq 0 ]]; then
    mapfile -t files < <(jq -r '.[].file' "$build_dir/compile_commands.json" |
        grep -vE '/(_deps|third_party)/' | sort -u)
else
    files=()
    for f in "$@"; do
        [[ $f =~ \.(cpp|cc)$ && -f $f ]] && files+=("$f")
    done
fi

[[ ${#files[@]} -eq 0 ]] && { echo "clang-tidy: nothing to check"; exit 0; }
printf '%s\n' "${files[@]}" | xargs -P "$(nproc)" -n 4 "$tidy" -p "$build_dir" --quiet

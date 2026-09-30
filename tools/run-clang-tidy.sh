#!/usr/bin/env bash
# Usage: tools/run-clang-tidy.sh <build-dir> [files...]
# With no files, checks every project translation unit in compile_commands.json.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

build_dir=$1
shift
tidy=${CLANG_TIDY:-clang-tidy-19}

if [[ $# -eq 0 ]]; then
    # Sources generated into the build tree (the bundled migrations) are data, and a tree built
    # only as far as its headers does not have them yet.
    build_abs=$(realpath "$build_dir")
    mapfile -t files < <(jq -r '.[].file' "$build_dir/compile_commands.json" |
        grep -vE '/(_deps|third_party)/' | grep -vF "$build_abs/" | sort -u)
else
    files=()
    for f in "$@"; do
        [[ $f =~ \.(cpp|cc)$ && -f $f ]] && files+=("$f")
    done
fi

[[ ${#files[@]} -eq 0 ]] && { echo "clang-tidy: nothing to check"; exit 0; }
# One file per process: batches of four left workers idle while the last batches ran.
printf '%s\n' "${files[@]}" | xargs -P "$(nproc)" -n 1 "$tidy" -p "$build_dir" --quiet

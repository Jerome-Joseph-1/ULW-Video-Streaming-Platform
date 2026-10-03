#!/usr/bin/env bash
# Usage: tools/run-clang-tidy.sh [--shard I/N] [--list] <build-dir> [files...]
# With no files, checks every project translation unit in compile_commands.json.
#   --shard I/N  check only the files at positions I, I+N, I+2N, ... (0 <= I < N) of the sorted
#                list, so N jobs with I = 0..N-1 together check every file exactly once.
#   --list       print the files that would be checked instead of checking them.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

shard_index=0
shard_count=1
list_only=false
while [[ $# -gt 0 ]]; do
    case $1 in
        --shard)
            [[ ${2-} =~ ^([0-9]+)/([0-9]+)$ ]] || { echo "clang-tidy: --shard wants I/N" >&2; exit 2; }
            shard_index=$((10#${BASH_REMATCH[1]}))
            shard_count=$((10#${BASH_REMATCH[2]}))
            ((shard_count > 0 && shard_index < shard_count)) ||
                { echo "clang-tidy: --shard $2 is not 0 <= I < N" >&2; exit 2; }
            shift 2
            ;;
        --list)
            list_only=true
            shift
            ;;
        *) break ;;
    esac
done

build_dir=$1
shift
tidy=${CLANG_TIDY:-clang-tidy-19}

if [[ $# -eq 0 ]]; then
    # Sources generated into the build tree (the bundled migrations) are data, and a tree built
    # only as far as its headers does not have them yet.
    build_abs=$(realpath "$build_dir")
    mapfile -t files < <(jq -r '.[].file' "$build_dir/compile_commands.json" |
        grep -vE '/(_deps|third_party)/' | grep -vF "$build_abs/" | LC_ALL=C sort -u)
else
    files=()
    while IFS= read -r f; do
        [[ $f =~ \.(cpp|cc)$ && -f $f ]] && files+=("$f")
    done < <(printf '%s\n' "$@" | LC_ALL=C sort -u)
fi

if ((shard_count > 1)); then
    picked=()
    for i in "${!files[@]}"; do
        ((i % shard_count == shard_index)) && picked+=("${files[i]}")
    done
    files=("${picked[@]}")
fi

if $list_only; then
    ((${#files[@]} == 0)) || printf '%s\n' "${files[@]}"
    exit 0
fi

[[ ${#files[@]} -eq 0 ]] && { echo "clang-tidy: nothing to check"; exit 0; }
echo "clang-tidy: ${#files[@]} files (shard $shard_index/$shard_count)"
# One file per process: batches of four left workers idle while the last batches ran.
printf '%s\n' "${files[@]}" | xargs -P "$(nproc)" -n 1 "$tidy" -p "$build_dir" --quiet

#!/usr/bin/env bash
# Checks that run-clang-tidy.sh --shard splits the file list into disjoint shards whose union is
# the unsharded list, for every shard count from 1 to 12.
# Usage: tools/run-clang-tidy_test.sh [build-dir]
# Without a build directory, runs against a synthetic compile_commands.json.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
script=tools/run-clang-tidy.sh

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [[ $# -gt 0 ]]; then
    build_dir=$1
else
    # Project sources, a duplicate entry, a generated source inside the build tree and a
    # dependency's source: only the project sources must come out, each once.
    build_dir=$work/build
    mkdir -p "$build_dir/generated" "$work/src/_deps/x"
    entries=()
    for i in $(seq 1 37); do
        entries+=("{\"directory\":\"$build_dir\",\"file\":\"$work/src/f$i.cpp\",\"command\":\"c++ -c f$i.cpp\"}")
    done
    entries+=("{\"directory\":\"$build_dir\",\"file\":\"$work/src/f7.cpp\",\"command\":\"c++ -c f7.cpp\"}")
    entries+=("{\"directory\":\"$build_dir\",\"file\":\"$build_dir/generated/m.cpp\",\"command\":\"c++ -c m.cpp\"}")
    entries+=("{\"directory\":\"$build_dir\",\"file\":\"$work/src/_deps/x/d.cpp\",\"command\":\"c++ -c d.cpp\"}")
    (IFS=,; printf '[%s]\n' "${entries[*]}") >"$build_dir/compile_commands.json"
fi

fail() { echo "run-clang-tidy_test: $*" >&2; exit 1; }

"$script" --list "$build_dir" >"$work/all"
total=$(wc -l <"$work/all")
((total > 0)) || fail "the unsharded list is empty"
[[ -z $(sort "$work/all" | uniq -d) ]] || fail "the unsharded list has duplicates"
if [[ $# -eq 0 ]]; then
    ((total == 37)) || fail "expected 37 project sources, got $total"
fi

for n in $(seq 1 12); do
    : >"$work/union"
    for ((i = 0; i < n; i++)); do
        "$script" --shard "$i/$n" --list "$build_dir" >"$work/shard"
        size=$(wc -l <"$work/shard")
        # Round-robin: shard sizes differ by at most one.
        ((size == total / n || size == total / n + 1)) || fail "shard $i/$n has $size of $total files"
        cat "$work/shard" >>"$work/union"
    done
    [[ -z $(sort "$work/union" | uniq -d) ]] || fail "shards of $n overlap"
    cmp -s <(sort "$work/all") <(sort "$work/union") || fail "shards of $n do not cover the list"
done

for bad in 3/3 1/0 x/2; do
    if "$script" --shard "$bad" --list "$build_dir" >/dev/null 2>&1; then
        fail "--shard $bad was accepted"
    fi
done
echo "run-clang-tidy_test: $total files, shards of 1..12 disjoint and complete"

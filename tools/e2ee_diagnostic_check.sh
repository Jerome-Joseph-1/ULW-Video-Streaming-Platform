#!/usr/bin/env bash
# Usage: tools/e2ee_diagnostic_check.sh [baseline]        baseline: a tag or commit, phase-2 by
#                                                         default
#
# End-to-end encryption must need nothing from the chat service or the room router (ADR-0043):
# from the baseline to the working tree, both files must hold the same code. This runs
# `git diff <baseline> -- <files>` and, when that is not empty, asks whether the difference is
# formatting only: each side, the baseline's blob and the working tree's file, goes through
# clang-format-18 with today's .clang-format, and the two results are compared byte for byte.
# Equal results mean the change was layout that clang-format decides (spaces, line breaks,
# include order, reflowed comments); anything else is a change, a reworded comment included,
# and so is a file added, deleted or renamed. `git diff -w` would not do: it calls a statement
# rewrapped over two lines a change, and `return x` and `returnx` the same.
#
# Exit status: 0 no change past formatting, 1 changes (both diffs printed), 2 bad usage or a
# baseline that does not exist.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

baseline=${1:-phase-2}
fmt=${CLANG_FORMAT:-clang-format-18}
files=(apps/chat/src/chat_service.cpp rt/src/room_router.cpp)

if [[ $# -gt 1 ]]; then
    echo "usage: $0 [baseline]" >&2
    exit 2
fi
if ! git rev-parse --verify --quiet "${baseline}^{commit}" >/dev/null; then
    echo "e2ee_diagnostic_check: no such baseline: $baseline" >&2
    exit 2
fi

raw=$(git diff "$baseline" -- "${files[@]}")
if [[ -z $raw ]]; then
    echo "e2ee_diagnostic_check: ${files[*]} unchanged since $baseline"
    exit 0
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

changed=()
for f in "${files[@]}"; do
    if ! git cat-file -e "$baseline:$f" 2>/dev/null || [[ ! -f $f ]]; then
        changed+=("$f")
        continue
    fi
    git show "$baseline:$f" | "$fmt" --style=file --assume-filename="$f" >"$scratch/before"
    "$fmt" --style=file --assume-filename="$f" <"$f" >"$scratch/after"
    if ! diff -u --label "$baseline:$f" --label "$f" "$scratch/before" "$scratch/after" \
        >>"$scratch/normalised"; then
        changed+=("$f")
    fi
done

if [[ ${#changed[@]} -eq 0 ]]; then
    echo "e2ee_diagnostic_check: ${files[*]} differ from $baseline in formatting only"
    exit 0
fi

{
    echo "e2ee_diagnostic_check: changed since $baseline: ${changed[*]}"
    echo
    echo "$raw"
    if [[ -s $scratch/normalised ]]; then
        echo
        echo "after clang-format on both sides:"
        cat "$scratch/normalised"
    fi
} >&2
exit 1

#!/usr/bin/env bash
# Usage: tools/e2ee_diagnostic_check.sh [<base> <head>]   tags or commits; phase-2 and phase-3
#                                                         by default
#
# End-to-end encryption must need nothing from the chat service or the room router (ADR-0043).
# M22 promises that the E2EE phase changed neither file: from phase-2, the tag that closes the
# chat phase, to phase-3, the tag that closes the E2EE phase, both files must hold the same code.
# Later phases may change them (M32 does), so the comparison ends at phase-3, not at HEAD.
# This runs `git diff <base> <head> -- <files>` and, when that is not empty, asks whether the
# difference is formatting only: each side's blob goes through clang-format-18 with today's
# .clang-format, and the two results are compared byte for byte. Equal results mean the change
# was layout that clang-format decides (spaces, line breaks, include order, reflowed comments);
# anything else is a change, a reworded comment included, and so is a file added, deleted or
# renamed. `git diff -w` would not do: it calls a statement rewrapped over two lines a change,
# and `return x` and `returnx` the same.
#
# Without arguments, and while either tag does not exist, it prints that it is waiting on both
# tags and succeeds. The tags are pushed by a person with tag rights; until both are on the
# remote, CI stays in that waiting state. With explicit refs, a missing ref is an error.
#
# Exit status: 0 no change past formatting (or waiting on the tags), 1 changes (both diffs
# printed), 2 bad usage or an explicit ref that does not exist.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

fmt=${CLANG_FORMAT:-clang-format-18}
files=(apps/chat/src/chat_service.cpp rt/src/room_router.cpp)

exists() { git rev-parse --verify --quiet "${1}^{commit}" >/dev/null; }

case $# in
0)
    base=phase-2
    head=phase-3
    if ! exists "$base" || ! exists "$head"; then
        echo "e2ee_diagnostic_check: waiting on both tags, $base and $head; they are pushed by" \
            "a person with tag rights, and nothing is compared until both exist"
        exit 0
    fi
    ;;
2)
    base=$1
    head=$2
    for ref in "$base" "$head"; do
        if ! exists "$ref"; then
            echo "e2ee_diagnostic_check: no such ref: $ref" >&2
            exit 2
        fi
    done
    ;;
*)
    echo "usage: $0 [<base> <head>]" >&2
    exit 2
    ;;
esac

raw=$(git diff "$base" "$head" -- "${files[@]}")
if [[ -z $raw ]]; then
    echo "e2ee_diagnostic_check: ${files[*]} unchanged from $base to $head"
    exit 0
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

changed=()
for f in "${files[@]}"; do
    if ! git cat-file -e "$base:$f" 2>/dev/null || ! git cat-file -e "$head:$f" 2>/dev/null; then
        changed+=("$f")
        continue
    fi
    git show "$base:$f" | "$fmt" --style=file --assume-filename="$f" >"$scratch/before"
    git show "$head:$f" | "$fmt" --style=file --assume-filename="$f" >"$scratch/after"
    if ! diff -u --label "$base:$f" --label "$head:$f" "$scratch/before" "$scratch/after" \
        >>"$scratch/normalised"; then
        changed+=("$f")
    fi
done

if [[ ${#changed[@]} -eq 0 ]]; then
    echo "e2ee_diagnostic_check: ${files[*]} differ from $base to $head in formatting only"
    exit 0
fi

{
    echo "e2ee_diagnostic_check: changed from $base to $head: ${changed[*]}"
    echo
    echo "$raw"
    if [[ -s $scratch/normalised ]]; then
        echo
        echo "after clang-format on both sides:"
        cat "$scratch/normalised"
    fi
} >&2
exit 1

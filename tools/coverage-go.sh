#!/usr/bin/env bash
# Usage: tools/coverage-go.sh <out>
# Runs the tests of every Go module in the repository (each tracked go.mod) with -coverprofile
# and writes one Go cover profile of the modules' product code for SonarQube Cloud (ADR-0079).
# The profile names each file by its import path (module path from go.mod, then the file); the
# Go analyser maps that back to the file through the module's go.mod. The modules use the
# standard library only, so nothing is fetched: GOPROXY=off and GOTOOLCHAIN=local make a module
# that started to need a download, or a newer toolchain, fail here rather than fetch one.
# Needs go on PATH (CI: the golang image, pinned by digest in sonar.yml).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
out=$(realpath -m "$1")
export GOTOOLCHAIN=local GOPROXY=off GOFLAGS=-mod=readonly
profiles=$(mktemp -d)
trap 'rm -rf "$profiles"' EXIT

failed=0
n=0
while IFS= read -r mod; do
    dir=$(dirname "$mod")
    n=$((n + 1))
    echo "coverage-go: $dir"
    (cd "$dir" && go test -count=1 -covermode=set -coverprofile="$profiles/$n.out" ./...) ||
        failed=1
done < <(git ls-files 'go.mod' '*/go.mod')

# One profile: the mode line once, then every module's blocks.
mkdir -p "$(dirname "$out")"
echo "mode: set" >"$out"
for profile in "$profiles"/*.out; do
    [[ -e $profile ]] && tail -n +2 "$profile" >>"$out"
done
if [[ $failed -ne 0 ]]; then
    echo "coverage-go: a test failed; the profile is not a measurement" >&2
    exit 1
fi

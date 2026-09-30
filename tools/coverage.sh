#!/usr/bin/env bash
# Usage: tools/coverage.sh <build-dir> [label...]
# In a tree built with the coverage preset: runs the ctest labels (unit, then integration, by
# default), merges the profiles every test process and every server it started wrote, and
# writes <build-dir>/coverage/ with html/, summary.json and summary.md. Tests, third-party and
# generated code are left out. Fails when a test fails or a top-level directory falls below its
# floor in tools/coverage-floors.txt (ADR-0074).
#
# The unit label runs with CTEST_PARALLEL_LEVEL jobs; every other label runs one test at a
# time, as the integration job runs it.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
root=$PWD

build_dir=$(realpath "$1")
shift
labels=("$@")
[[ ${#labels[@]} -eq 0 ]] && labels=(unit integration)
llvm=${LLVM_SUFFIX:--19}
out=$build_dir/coverage

rm -rf "$out"
mkdir -p "$out/profiles"
# The same path the preset builds into every binary (cmake/Coverage.cmake), which is where the
# servers the tests start write theirs.
export LLVM_PROFILE_FILE=$out/profiles/ulw-%8m.profraw

failed=0
for label in "${labels[@]}"; do
    jobs=1
    [[ $label == unit ]] && jobs=${CTEST_PARALLEL_LEVEL:-$(nproc)}
    ctest --test-dir "$build_dir" -L "^${label}\$" -j "$jobs" --no-tests=error \
        --output-on-failure || failed=1
done

"llvm-profdata$llvm" merge -sparse -o "$out/ulw.profdata" "$out"/profiles/*.profraw

# Every instrumented binary, test suites and the servers they start alike; a source file only
# a binary no test ran is counted, at zero.
objects=()
while IFS= read -r -d '' f; do
    grep -qa __llvm_covmap "$f" && objects+=("$f")
done < <(find "$build_dir" -path "$build_dir/_deps" -prune -o -type f -perm -u+x -print0)
[[ ${#objects[@]} -gt 0 ]] || { echo "coverage: no instrumented binaries in $build_dir" >&2; exit 1; }

cov_args=(-instr-profile="$out/ulw.profdata" "${objects[0]}")
for f in "${objects[@]:1}"; do cov_args+=(-object="$f"); done
# The first-party directories; tests/ and third_party/ are not among them, and the build tree's
# generated sources (build info, the bundled migrations) are outside all of them.
sources=()
for d in apps codec core http infra net ops os rt tools; do
    [[ -d $d ]] && sources+=("$root/$d")
done
ignore="^$root/(tests|third_party|build)/"

"llvm-cov$llvm" export -format=text -summary-only -ignore-filename-regex="$ignore" \
    "${cov_args[@]}" "${sources[@]}" >"$out/export.json"
"llvm-cov$llvm" show -format=html -show-branches=count -show-line-counts-or-regions \
    -ignore-filename-regex="$ignore" -output-dir="$out/html" -project-title=ulw \
    "${cov_args[@]}" "${sources[@]}"

report_status=0
python3 tools/coverage_report.py "$root" "$out/export.json" tools/coverage-floors.txt \
    "$out/summary.json" "$out/summary.md" || report_status=$?
[[ -n ${GITHUB_STEP_SUMMARY:-} ]] && cat "$out/summary.md" >>"$GITHUB_STEP_SUMMARY"
cat "$out/summary.md"

if [[ $failed -ne 0 ]]; then
    echo "coverage: a test failed; the numbers above are not a measurement" >&2
    exit 1
fi
exit "$report_status"

#!/usr/bin/env bash
# Usage: tools/coverage.sh <build-dir> [label...]
# In a tree built with the coverage preset: runs the ctest labels (unit, then integration, by
# default), merges the profiles every test process and every server it started wrote, and
# writes <build-dir>/coverage/ with html/, summary.json and summary.md. Tests, third-party and
# generated code are left out. Fails when a test fails and, unless COVERAGE_ENFORCE=0, when a
# top-level directory falls below its floor in tools/coverage-floors.txt (ADR-0080). With
# COVERAGE_ENFORCE=0 the floor comparison is printed as a warning and only the tests decide.
# With COVERAGE_SONAR=1 it also writes <build-dir>/coverage/llvm-cov-show.txt, the `llvm-cov
# show` text report SonarQube Cloud reads (sonar.cfamily.llvm-cov.reportPath, ADR-0079).
#
# Split across machines (ADR-0083), in two steps:
#   COVERAGE_SHARD=i/n runs every n-th test of each label from its i-th (ctest -I i,,n), so the
#     n shards together run every test exactly once, and merges what they measured into
#     <build-dir>/coverage/ulw.profdata. No report: one shard's numbers are a fraction.
#   COVERAGE_PROFILES=<dir> runs no test: it merges every *.profdata under <dir> (the shards')
#     and reports on that, over this build's binaries. They must be the shards' build: the same
#     sources, preset and clang, which is what makes the profiles match the binaries.
#
# Every label runs one test at a time, as CI's other jobs run them: several suites time servers
# and the database, and a busy runner would change what they measure.
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

enforce=${COVERAGE_ENFORCE:-1}
[[ $enforce == 0 || $enforce == 1 ]] || { echo "coverage: COVERAGE_ENFORCE is 0 or 1" >&2; exit 2; }
sonar=${COVERAGE_SONAR:-0}
[[ $sonar == 0 || $sonar == 1 ]] || { echo "coverage: COVERAGE_SONAR is 0 or 1" >&2; exit 2; }
shard=${COVERAGE_SHARD:-}
shard_args=()
if [[ -n $shard ]]; then
    if ! [[ $shard =~ ^([1-9][0-9]*)/([1-9][0-9]*)$ ]] || ((BASH_REMATCH[1] > BASH_REMATCH[2])); then
        echo "coverage: COVERAGE_SHARD is i/n with 1 <= i <= n" >&2
        exit 2
    fi
    shard_args=(-I "${BASH_REMATCH[1]},,${BASH_REMATCH[2]}")
fi
profiles=${COVERAGE_PROFILES:-}
if [[ -n $shard && -n $profiles ]]; then
    echo "coverage: COVERAGE_SHARD and COVERAGE_PROFILES are two steps, not one" >&2
    exit 2
fi

failed=0
if [[ -n $profiles ]]; then
    mapfile -d '' -t merged < <(find "$profiles" -type f -name '*.profdata' -print0 | sort -z)
    [[ ${#merged[@]} -gt 0 ]] || { echo "coverage: no *.profdata under $profiles" >&2; exit 1; }
    echo "coverage: merging ${#merged[@]} profiles from $profiles" >&2
    "llvm-profdata$llvm" merge -sparse -o "$out/ulw.profdata" "${merged[@]}"
else
    for label in "${labels[@]}"; do
        # The same per-test limit as the ci test preset. With a shard, -I takes every n-th
        # test of the ones the label selects.
        ctest --test-dir "$build_dir" -L "^${label}\$" "${shard_args[@]}" -j 1 --timeout 600 \
            --no-tests=error --output-on-failure || failed=1
    done
    "llvm-profdata$llvm" merge -sparse -o "$out/ulw.profdata" "$out"/profiles/*.profraw
fi

if [[ -n $shard ]]; then
    rm -rf "$out/profiles"
    if [[ $failed -ne 0 ]]; then
        echo "coverage: a test failed in shard $shard; its profile is not a measurement" >&2
        exit 1
    fi
    exit 0
fi

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
# The form SonarSource's CFamily coverage example uses: `llvm-cov show --show-branches=count`,
# plain text, every object. Absolute paths, which the scanner maps onto the checkout.
# Instantiations are folded into each line's count, which is what a line's coverage is.
if [[ $sonar == 1 ]]; then
    "llvm-cov$llvm" show -format=text -show-branches=count -show-instantiations=false \
        -ignore-filename-regex="$ignore" "${cov_args[@]}" "${sources[@]}" \
        >"$out/llvm-cov-show.txt"
fi

report_status=0
python3 tools/coverage_report.py "$root" "$out/export.json" tools/coverage-floors.txt \
    "$out/summary.json" "$out/summary.md" || report_status=$?
[[ -n ${GITHUB_STEP_SUMMARY:-} ]] && cat "$out/summary.md" >>"$GITHUB_STEP_SUMMARY"
cat "$out/summary.md"

# 3 is a directory below its floor; anything else is the report failing.
if [[ $report_status -eq 3 && $enforce == 0 ]]; then
    msg="a directory is below its floor (COVERAGE_ENFORCE=0: reported, not enforced)"
    echo "coverage: warning: $msg" >&2
    [[ -n ${GITHUB_ACTIONS:-} ]] && echo "::warning title=coverage::$msg"
    report_status=0
fi
if [[ $failed -ne 0 ]]; then
    echo "coverage: a test failed; the numbers above are not a measurement" >&2
    exit 1
fi
exit "$report_status"

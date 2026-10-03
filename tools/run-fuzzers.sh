#!/usr/bin/env bash
# Usage: tools/run-fuzzers.sh <build-dir> <work-dir> <seconds> <target>...
# Runs each <target>_fuzz for <seconds> on its committed corpus (tests/fuzz/corpus/<target>),
# copied into <work-dir>/<target>/corpus so what a run discovers never lands in the checkout.
# libFuzzer runs single-threaded, so nproc targets run at once, each with a core to itself.
# Crash inputs go to <work-dir>/<target>/artifacts/, the output to <work-dir>/<target>/fuzz.log,
# printed per target afterwards. Exits non-zero if any target failed.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

build_dir=$(realpath "$1")
work_dir=$2
seconds=$3
shift 3
[[ $# -gt 0 ]] || { echo "run-fuzzers: no targets" >&2; exit 2; }
mkdir -p "$work_dir"
work_dir=$(realpath "$work_dir")

run_one() {
    local target=$1 dir=$work_dir/$1 status=0
    mkdir -p "$dir/corpus" "$dir/artifacts"
    {
        cp "tests/fuzz/corpus/$target"/* "$dir/corpus/" &&
            "$build_dir/tests/${target}_fuzz" -max_total_time="$seconds" -print_final_stats=1 \
                -artifact_prefix="$dir/artifacts/" "$dir/corpus"
    } >"$dir/fuzz.log" 2>&1 || status=$?
    if ((status == 0)); then
        echo "$target: ok"
    else
        echo "$target: FAILED (exit $status)"
        touch "$dir/failed"
    fi
}

jobs_max=$(nproc)
for target in "$@"; do
    while (($(jobs -rp | wc -l) >= jobs_max)); do
        wait -n || true
    done
    run_one "$target" &
done
wait

status=0
for target in "$@"; do
    echo "::group::fuzz ($target)"
    cat "$work_dir/$target/fuzz.log"
    echo "::endgroup::"
    if [[ -e $work_dir/$target/failed ]]; then
        echo "::error::fuzz ($target) failed; see its log and $work_dir/$target/artifacts"
        status=1
    fi
done
exit "$status"

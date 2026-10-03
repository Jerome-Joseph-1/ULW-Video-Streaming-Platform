#!/usr/bin/env bash
# Usage: tools/coverage-python.sh <out.xml>
# Runs every Python unit test in the repository (each tracked *_test.py, as its own process, as
# CI runs them) under coverage.py and writes a Cobertura XML report of the Python product code
# (deploy/ and tools/; tests are not measured) for SonarQube Cloud (ADR-0079). Settings are in
# tools/coverage-python.rc; paths in the report are relative to the repository root.
# Needs coverage.py for the python3 on PATH (CI: Ubuntu's python3-coverage, pinned in ci.yml's
# coverage job).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
out=$(realpath -m "$1")
export COVERAGE_RCFILE=$PWD/tools/coverage-python.rc
# The data files go in a directory of their own, so a stale one can never be combined.
data=$(mktemp -d)
trap 'rm -rf "$data"' EXIT
export COVERAGE_FILE=$data/.coverage

failed=0
while IFS= read -r test; do
    echo "coverage-python: $test"
    python3 -m coverage run --parallel-mode "$test" || failed=1
done < <(git ls-files '*_test.py')

python3 -m coverage combine --quiet "$data"
mkdir -p "$(dirname "$out")"
python3 -m coverage xml -o "$out"
python3 -m coverage report
if [[ $failed -ne 0 ]]; then
    echo "coverage-python: a test failed; the numbers above are not a measurement" >&2
    exit 1
fi

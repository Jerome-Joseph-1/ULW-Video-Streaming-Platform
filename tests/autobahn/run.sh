#!/usr/bin/env bash
# Usage: tests/autobahn/run.sh [build-dir]
# Runs the Autobahn fuzzingclient against ulw_ws_echo_server from build-dir (default build/ci)
# and fails unless check_results.py accepts the report: every case's behavior and behaviorClose
# OK, NON-STRICT or INFORMATIONAL, or UNIMPLEMENTED for the compression cases (12.x, 13.x),
# and no fewer cases than the pinned image runs.
# Reports land in <build-dir>/autobahn/reports.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

build_dir=${1:-build/ci}
port=${ULW_AUTOBAHN_PORT:-9001}
# crossbario/autobahn-testsuite 25.10.1 (vcs-ref 6ed6f43), the latest tag on 2026-09-29.
image=crossbario/autobahn-testsuite@sha256:519915fb568b04c9383f70a1c405ae3ff44ab9e35835b085239c258b6fac3074
# The number of cases that image runs; fewer means the suite stopped early or ran nothing.
min_cases=517
out=$build_dir/autobahn

rm -rf "$out"
mkdir -p "$out"
sed "s/@PORT@/$port/" tests/autobahn/fuzzingclient.json >"$out/fuzzingclient.json"

# The server prints one line once it listens; reading it is the readiness check.
exec 3< <(exec "$build_dir/tests/ulw_ws_echo_server" --port "$port" 2>&1)
server=$!
trap 'kill "$server" 2>/dev/null || true' EXIT
read -r ready <&3
echo "$ready"
[[ $ready == listening* ]] || exit 1

# Host networking: the suite dials the server on the host's loopback.
docker run --rm --network host -v "$PWD/$out:/work" -w /work "$image" \
    wstest -m fuzzingclient -s /work/fuzzingclient.json >"$out/wstest.log"

python3 tests/autobahn/check_results.py "$out/reports/index.json" "$min_cases"

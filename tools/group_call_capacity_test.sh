#!/usr/bin/env bash
# M29 (ADR-0095): a group call's egress on the SFU against its derivation, n x (n - 1) streams of
# 700 kbps out per call, within 10%, with the layer stated (one layer, simulcast off: every
# subscriber gets the top one). Rooms of four by default; the measured CPU per call sits beside it
# in the report, for the capacity table in ADR-0095.
#   tools/group_call_capacity_test.sh [--participants 4] [--calls 2] [--seconds 60] [--out DIR]
# Needs Docker, root (the SFU container's counters are read from /proc) and Python 3.11 or 3.12;
# the LiveKit Python SDK the clients use is installed, by hash, into a venv of its own
# (ULW_CALLCAP_VENV, default .venv-callcap at the repository's root) the first time.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
venv=${ULW_CALLCAP_VENV:-$root/.venv-callcap}
if [[ ! -x $venv/bin/python ]]; then
    python3 -m venv "$venv"
    "$venv/bin/pip" install -q --require-hashes --only-binary :all: \
        -r "$root/tests/load/call_capacity/requirements.txt"
fi
args=("$@")
[[ " ${args[*]} " == *" --participants "* ]] || args+=(--participants 4)
[[ " ${args[*]} " == *" --calls "* ]] || args+=(--calls 2)
exec "$venv/bin/python" "$root/tests/load/call_capacity/call_capacity.py" "${args[@]}"

#!/usr/bin/env bash
# Follows the logs of what tools/dev/up.sh started: tools/dev/logs.sh [gateway|worker|chat|migrate]
set -euo pipefail
# shellcheck source=tools/dev/lib.sh
. "$(dirname "$0")/lib.sh"

if (($#)); then
    exec tail -n 50 -F "${@/#/$dev_state/logs/}"
fi
exec tail -n 20 -F "$dev_state"/logs/{gateway,worker,chat}.log

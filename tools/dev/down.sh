#!/usr/bin/env bash
# Stops what tools/dev/up.sh started. The backing services keep running (tools/dev/services.sh
# down stops them).
set -euo pipefail
# shellcheck source=tools/dev/lib.sh
. "$(dirname "$0")/lib.sh"

for name in chat worker gateway; do
    pidfile=$dev_state/run/$name.pid
    [[ -f $pidfile ]] || continue
    pid=$(cat "$pidfile")
    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid"
        for _ in $(seq 1 100); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$pid" 2>/dev/null; then
            echo "down: $name did not stop in 10 s; killing it" >&2
            kill -KILL "$pid"
        fi
        echo "down: stopped $name"
    fi
    rm -f "$pidfile"
done

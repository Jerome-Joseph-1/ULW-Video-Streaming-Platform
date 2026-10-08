#!/usr/bin/env bash
# Stops the demo. Videos, messages and the signing key stay for the next up.sh, unless:
#   demo/down.sh --wipe     also deletes them (the demo's Docker volumes)
set -euo pipefail
cd "$(dirname "$0")"
if [[ ${1:-} == --wipe ]]; then
    docker compose down --volumes --remove-orphans
else
    docker compose down --remove-orphans
fi

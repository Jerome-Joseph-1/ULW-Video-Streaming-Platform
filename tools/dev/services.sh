#!/usr/bin/env bash
# The local backing services of the development environment: deploy/local/compose.yaml with
# .devcontainer/compose.persist.yaml over it, so their data outlives a restart.
#
#   tools/dev/services.sh up       Postgres and MinIO (post-start.sh runs this)
#   tools/dev/services.sh calls    also LiveKit, its Redis and the recorder (egress)
#   tools/dev/services.sh status
#   tools/dev/services.sh down     stops them; the data stays
#   tools/dev/services.sh reset    stops them and deletes their data
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
compose=(docker compose -f "$root/deploy/local/compose.yaml"
    -f "$root/.devcontainer/compose.persist.yaml")

case ${1:-up} in
up) "${compose[@]}" up -d --wait postgres minio ;;
calls) "${compose[@]}" --profile calls up -d --wait ;;
status) "${compose[@]}" --profile calls ps ;;
down) "${compose[@]}" --profile calls down ;;
reset) "${compose[@]}" --profile calls down --volumes ;;
*)
    sed -n '2,10p' "$0" >&2
    exit 2
    ;;
esac

#!/usr/bin/env bash
# Removes everything e2e-up.sh and deploy/stunner/up.sh created: the kind cluster and the
# network outside it, the Postgres and MinIO containers (their data was tmpfs), the images they
# put in the local image store, and deploy/local/.state with the sandbox CA and kubeconfig.
# The tools in .tools stay; they are pinned and verified.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
tools=$("$here/tools.sh")
# shellcheck source=deploy/local/images.sh
source "$here/images.sh"

# shellcheck source=deploy/local/sandbox.sh
source "$here/sandbox.sh"
"$tools/kind" delete cluster --name "$cluster" --kubeconfig "$kubeconfig"
docker rm --force ulw-e2e-pg ulw-e2e-minio >/dev/null 2>&1 || true
# Postgres and MinIO stay: compose.yaml runs the same images. So does LiveKit's content: only
# its tag goes, and the call suite's compose server runs the same digest (images.sh). Until
# that suite is on this branch, the ~100 MB it leaves is the price of not pulling it again.
docker image rm "${built_images[@]}" "$eg_image" "${eg_image%:*}@$eg_digest" "$envoy_image" \
    "${envoy_image%:*}@$envoy_digest" "$kube_router_image" \
    "${kube_router_image%:*}@$kube_router_digest" "$metrics_server_image" \
    "${ULW_METRICS_SERVER_REPO:-$metrics_server_repo}@$metrics_server_digest" \
    "$stunner_operator_image" "${stunner_operator_image%:*}@$stunner_operator_digest" \
    "$stunnerd_image" "${stunnerd_image%:*}@$stunnerd_digest" "$livekit_image" \
    "$livekit_redis_image" "$probe_image" \
    >/dev/null 2>&1 || true
docker network rm "$outside_network" >/dev/null 2>&1 || true
rm -rf "$here/.state"

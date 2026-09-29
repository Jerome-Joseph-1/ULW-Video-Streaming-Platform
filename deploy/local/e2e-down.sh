#!/usr/bin/env bash
# Removes everything e2e-up.sh created: the kind cluster, the Postgres and MinIO containers
# (their data was tmpfs), the images it put in the local image store, and deploy/local/.state
# with the sandbox CA and kubeconfig. The tools in .tools stay; they are pinned and verified.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
tools=$("$here/tools.sh")
# shellcheck source=deploy/local/images.sh
source "$here/images.sh"

# shellcheck source=deploy/local/sandbox.sh
source "$here/sandbox.sh"
"$tools/kind" delete cluster --name "$cluster" --kubeconfig "$kubeconfig"
docker rm --force ulw-e2e-pg ulw-e2e-minio >/dev/null 2>&1 || true
# Postgres and MinIO stay: compose.yaml runs the same images.
docker image rm "${built_images[@]}" "$eg_image" "${eg_image%:*}@$eg_digest" "$envoy_image" \
    "${envoy_image%:*}@$envoy_digest" "$kube_router_image" \
    "${kube_router_image%:*}@$kube_router_digest" >/dev/null 2>&1 || true
rm -rf "$here/.state"

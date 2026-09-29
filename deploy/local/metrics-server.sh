#!/usr/bin/env bash
# Installs metrics-server v0.9.0 into the sandbox cluster, for `kubectl top` (the load check,
# tests/cluster/load_check.py). The manifest is pinned by SHA-256 (tools.sh) and the image by
# digest (images.sh), pulled and loaded into the node as e2e-up.sh does for the others.
# Rerunning is harmless.
#
#   ULW_METRICS_SERVER_REPO  pull the image by digest from this repository instead of
#                            registry.k8s.io, on machines that cannot reach it; the digest
#                            still has to match, and the node files it under the pinned tag
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
tools=$("$here/tools.sh")
# shellcheck source=deploy/local/sandbox.sh
source "$here/sandbox.sh"
# shellcheck source=deploy/local/images.sh
source "$here/images.sh"
require_sandbox

source_repo=${ULW_METRICS_SERVER_REPO:-$metrics_server_repo}
archive=$(mktemp --suffix=.tar)
trap 'rm -f "$archive"' EXIT
docker pull --quiet "$source_repo@$metrics_server_digest" >/dev/null
docker tag "$source_repo@$metrics_server_digest" "$metrics_server_image"
docker save --platform linux/amd64 --output "$archive" "$metrics_server_image"
"$tools/kind" load image-archive --name "$cluster" "$archive"

# The manifest's tag would be pulled from the registry; the node has the pinned content under it.
kubectl apply -f "$tools/metrics-server.yaml" >/dev/null
# Sandbox only: the kind node's kubelet serves a certificate its own CA signed, which
# metrics-server cannot verify, so it is told to skip the check. A real cluster's kubelets
# have certificates the cluster CA vouches for and must never run with this flag.
args=$(kubectl -n kube-system get deployment metrics-server \
    -o jsonpath='{.spec.template.spec.containers[0].args}')
if [[ $args != *--kubelet-insecure-tls* ]]; then
    kubectl -n kube-system patch deployment metrics-server --type=json -p '[
      {"op":"add","path":"/spec/template/spec/containers/0/args/-","value":"--kubelet-insecure-tls"},
      {"op":"replace","path":"/spec/template/spec/containers/0/imagePullPolicy","value":"Never"}]' \
        >/dev/null
fi
kubectl -n kube-system rollout status deployment/metrics-server --timeout=180s
kubectl wait --for=condition=Available apiservice/v1beta1.metrics.k8s.io --timeout=180s
echo "metrics-server: up" >&2

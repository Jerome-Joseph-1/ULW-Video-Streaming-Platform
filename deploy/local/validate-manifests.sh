#!/usr/bin/env bash
# Validates every manifest this repository ships or applies, offline, with kubeconform against
# pinned schemas: Kubernetes v1.37.0 for the built-in kinds and the CRDs of the pinned Envoy
# Gateway release (Gateway API included) for the rest. A kind without a schema fails.
#
# With --server, it also asks the sandbox cluster's API server (e2e-up.sh must have run) to
# dry-run the Askedin overlays, prod and stage, which checks them against the admission chain
# and the CRDs as installed. Only the sandbox: its kubeconfig is the only one this script uses.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/deploy/local
tools=$("$here/tools.sh")

python3 "$here/crd-schemas.py" "$tools/schemas/crd" "$tools/envoy-gateway.yaml"

manifests=$(mktemp -d)
trap 'rm -rf "$manifests"' EXIT
"$tools/kubectl" kustomize --load-restrictor LoadRestrictionsNone "$here/cluster" \
    >"$manifests/sandbox.yaml"

"$tools/kubeconform" -strict -summary -output text \
    -schema-location "$tools/schemas/{{.ResourceKind}}{{.KindSuffix}}.json" \
    -schema-location "$tools/schemas/crd/{{.Group}}/{{.ResourceKind}}_{{.ResourceAPIVersion}}.json" \
    "$root/deploy/askedin/overlays" "$manifests/sandbox.yaml"

if [[ ${1:-} == --server ]]; then
    export KUBECONFIG=$here/.state/kubeconfig
    for env in stage prod; do
        # The prod overlays name namespace apps, which the sandbox has; server-side dry runs
        # persist nothing.
        find "$root/deploy/askedin/overlays/$env" -name '*.yaml' -print0 |
            xargs -0 -n1 "$tools/kubectl" apply --dry-run=server -f
    done
fi

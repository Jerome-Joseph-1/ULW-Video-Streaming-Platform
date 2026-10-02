#!/usr/bin/env bash
# Validates every manifest this repository ships or applies, offline, with kubeconform against
# pinned schemas: Kubernetes v1.37.0 for the built-in kinds, and the CRDs of the pinned Envoy
# Gateway (Gateway API included) and STUNner releases for the rest. A kind without a schema
# fails. The Woodpecker pipeline goes through woodpecker-cli's linter, with its buildx plugin
# privileged as RUNBOOK.md has the Woodpecker server configure it.
#
# With --server, it also asks the sandbox cluster's API server (e2e-up.sh must have run) to
# dry-run the Askedin overlays, prod and stage, and STUNner's manifests, which checks them
# against the admission chain and the CRDs as installed. Only the sandbox: its kubeconfig is
# the only one this script uses.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/deploy/local
tools=$("$here/tools.sh")

python3 "$here/crd-schemas.py" "$tools/schemas/crd" "$tools/envoy-gateway.yaml" \
    "$tools/stunner-crds.yaml"

manifests=$(mktemp -d)
trap 'rm -rf "$manifests"' EXIT
"$tools/kubectl" kustomize --load-restrictor LoadRestrictionsNone "$here/cluster" \
    >"$manifests/sandbox.yaml"
"$tools/kubectl" kustomize --load-restrictor LoadRestrictionsNone "$root/deploy/stunner" \
    >"$manifests/stunner.yaml"

"$tools/kubeconform" -strict -summary -output text \
    -schema-location "$tools/schemas/{{.ResourceKind}}{{.KindSuffix}}.json" \
    -schema-location "$tools/schemas/crd/{{.Group}}/{{.ResourceKind}}_{{.ResourceAPIVersion}}.json" \
    "$root/deploy/askedin/overlays" "$root/deploy/askedin/stunner" "$manifests/sandbox.yaml" \
    "$manifests/stunner.yaml"

# Every image by digest (check-image-pins.py): what Askedin runs and what the host runs
# directly, and, for the sandbox's kustomizations as rendered, a tag images.sh pins to one.
# deploy/askedin holds no kustomization (its overlays are plain manifests applied as they are),
# so its files are checked as written; one added there would need rendering here first.
if find "$root/deploy/askedin" -name kustomization.yaml | grep -q .; then
    echo "validate-manifests: deploy/askedin has a kustomization; render it for check-image-pins" >&2
    exit 1
fi
mapfile -t askedin_files < <(find "$root/deploy/askedin" \( -name '*.yaml' -o -name '*.yml' \) \
    -print | sort)
python3 "$here/check-image-pins.py" --images-sh "$here/images.sh" \
    --askedin "${askedin_files[@]}" \
    --host "$here/compose.yaml" "$here/kind.yaml" \
    --sandbox "$manifests/sandbox.yaml" "$manifests/stunner.yaml"

buildx=$(grep -om1 'woodpeckerci/plugin-docker-buildx:[^ ]*' "$root/deploy/askedin/woodpecker.yml")
"$tools/woodpecker-cli" --disable-update-check lint --strict --plugins-privileged "$buildx" \
    "$root/deploy/askedin/woodpecker.yml"

if [[ ${1:-} == --server ]]; then
    # shellcheck source=deploy/local/sandbox.sh
    source "$here/sandbox.sh"
    require_sandbox
    # The prod overlays name namespace apps, which the sandbox has; server-side dry runs
    # persist nothing.
    while IFS= read -r -d '' manifest; do
        kubectl apply --dry-run=server -f "$manifest"
    done < <(find "$root/deploy/askedin/overlays" "$root/deploy/askedin/stunner" -name '*.yaml' \
        -print0 | sort -z)
fi

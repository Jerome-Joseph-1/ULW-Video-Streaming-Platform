#!/usr/bin/env bash
# Validates every manifest this repository ships or applies, offline, with kubeconform against
# pinned schemas: Kubernetes v1.37.0 for the built-in kinds, and the CRDs of the pinned Envoy
# Gateway (Gateway API included) and STUNner releases for the rest. A kind without a schema
# fails. The Woodpecker pipeline goes through woodpecker-cli's linter, with its buildx plugin
# privileged as RUNBOOK.md has the Woodpecker server configure it.
#
# The live packager's Job (deploy/askedin/live-packager/job.yaml) is a template, filled in per
# stream; it is checked here as filled in for a sample stream in each environment.
#
# With --server, it also asks the sandbox cluster's API server (e2e-up.sh must have run) to
# dry-run the Askedin overlays, prod and stage, the packager's Job as above, and STUNner's
# manifests, which checks them against the admission chain and the CRDs as installed. Only the
# sandbox: its kubeconfig is the only one this script uses.
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
# As RUNBOOK.md fills it with envsubst, for these variables only ($(POD_IP) stays as written).
# shellcheck disable=SC2016 # the template's literal ${...}, not the shell's
packager_job() {
    sed -e "s/\${ULW_NAMESPACE}/$1/g" -e "s/\${ULW_IMAGE_TAG}/$2/g" \
        -e 's/\${ULW_STREAM_ID}/sample-stream/g' \
        -e 's/\${ULW_STREAM_OWNER}/0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d/g' \
        "$root/deploy/askedin/live-packager/job.yaml"
}
# Stage may follow main; prod names a commit published from main (RUNBOOK.md, step 9).
packager_job apps-stage main >"$manifests/live-packager-stage.yaml"
packager_job apps 0123456789abcdef0123456789abcdef01234567 >"$manifests/live-packager-prod.yaml"
# shellcheck disable=SC2016 # a literal ${
if grep -n '\${' "$manifests"/live-packager-*.yaml; then
    echo "validate-manifests: live-packager/job.yaml has a variable RUNBOOK.md does not fill" >&2
    exit 1
fi

"$tools/kubeconform" -strict -summary -output text \
    -schema-location "$tools/schemas/{{.ResourceKind}}{{.KindSuffix}}.json" \
    -schema-location "$tools/schemas/crd/{{.Group}}/{{.ResourceKind}}_{{.ResourceAPIVersion}}.json" \
    "$root/deploy/askedin/overlays" "$root/deploy/askedin/stunner" "$manifests/sandbox.yaml" \
    "$manifests/stunner.yaml" "$manifests/live-packager-stage.yaml" \
    "$manifests/live-packager-prod.yaml"

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
    kubectl apply --dry-run=server -f "$manifests/live-packager-stage.yaml" \
        -f "$manifests/live-packager-prod.yaml"
fi

#!/usr/bin/env bash
# Validates every manifest this repository ships or applies, offline, with kubeconform against
# pinned schemas: Kubernetes v1.37.0 for the built-in kinds, and the CRDs of the pinned Envoy
# Gateway (Gateway API included) and STUNner releases for the rest. A kind without a schema
# fails. deploy/kubernetes is checked as written (the base and the once-per-cluster manifests)
# and as each example overlay renders it, from its config.env; so is the sandbox, from its own.
#
# The live packager's Job (deploy/kubernetes/live-packager/job.yaml) is a template, filled in
# per stream from an overlay's config.env; it is checked as filled in for a sample stream from
# each example overlay's.
#
# With --server, it also asks the sandbox cluster's API server (e2e-up.sh must have run) to
# dry-run the example overlays as rendered, the packager's Job as above, and STUNner's
# manifests, which checks them against the admission chain and the CRDs as installed. Only the
# sandbox: its kubeconfig is the only one this script uses.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/deploy/local
kube=$root/deploy/kubernetes
tools=$("$here/tools.sh")

python3 "$here/crd-schemas.py" "$tools/schemas/crd" "$tools/envoy-gateway.yaml" \
    "$tools/stunner-crds.yaml"

manifests=$(mktemp -d)
trap 'rm -rf "$manifests"' EXIT
render() { "$tools/kubectl" kustomize --load-restrictor LoadRestrictionsNone "$1"; }
render "$here/cluster" >"$manifests/sandbox.yaml"
render "$root/deploy/stunner" >"$manifests/stunner.yaml"
for overlay in staging production; do
    render "$kube/overlays/$overlay" >"$manifests/$overlay.yaml"
done
# As RUNBOOK.md (step 9) fills it with envsubst, from the overlay's config.env and a sample
# stream, for exactly these variables ($(POD_IP) stays as written); in Python, since envsubst
# is not on every machine that runs this.
packager_job() {
    (
        set -a
        # shellcheck source=deploy/kubernetes/overlays/staging/config.env
        source "$kube/overlays/$1/config.env"
        export ULW_STREAM_ID=sample-stream
        export ULW_STREAM_OWNER=0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d
        python3 - "$kube/live-packager/job.yaml" <<'EOF'
import os
import sys

NAMES = ["LIVE_NAMESPACE", "LIVE_PACKAGER_IMAGE_TAG", "IMAGE_PULL_POLICY", "STORAGE", "R2_ACCOUNT_ID",
         "S3_ENDPOINT", "BUCKET", "LIVE_PACKAGER_SECRET", "ULW_STREAM_ID", "ULW_STREAM_OWNER"]
with open(sys.argv[1], encoding="utf-8") as f:
    text = f.read()
for name in NAMES:
    text = text.replace("${" + name + "}", os.environ[name])
sys.stdout.write(text)
EOF
    )
}
for overlay in staging production; do
    packager_job "$overlay" >"$manifests/live-packager-$overlay.yaml"
done
# shellcheck disable=SC2016 # a literal ${
if grep -n '\${' "$manifests"/live-packager-*.yaml; then
    echo "validate-manifests: live-packager/job.yaml has a variable RUNBOOK.md does not fill" >&2
    exit 1
fi

"$tools/kubeconform" -strict -summary -output text \
    -schema-location "$tools/schemas/{{.ResourceKind}}{{.KindSuffix}}.json" \
    -schema-location "$tools/schemas/crd/{{.Group}}/{{.ResourceKind}}_{{.ResourceAPIVersion}}.json" \
    -ignore-filename-pattern 'kustomization\.yaml$' \
    "$kube/base" "$kube/cluster/stunner" "$manifests"/*.yaml

# Every image by digest (check-image-pins.py): deploy/kubernetes as written and its staging
# overlay as rendered may name this repository's builds by any tag, the production overlay as
# rendered (and its packager) only by a published commit; and what the host runs directly, and
# the sandbox's kustomizations as rendered, by a tag images.sh pins to one.
mapfile -t kube_files < <(find "$kube/base" "$kube/cluster" "$kube/live-packager" \
    \( -name '*.yaml' -o -name '*.yml' \) -not -name kustomization.yaml -print | sort)
python3 "$here/check-image-pins.py" --images-sh "$here/images.sh" \
    --kubernetes "${kube_files[@]}" "$manifests/staging.yaml" \
    --pinned "$manifests/production.yaml" "$manifests/live-packager-production.yaml" \
    --host "$here/compose.yaml" "$here/kind.yaml" \
    --sandbox "$manifests/sandbox.yaml" "$manifests/stunner.yaml"

if [[ ${1:-} == --server ]]; then
    # shellcheck source=deploy/local/sandbox.sh
    source "$here/sandbox.sh"
    require_sandbox
    # The overlays name namespaces of their own, which a dry run cannot create, so they are
    # created in the sandbox, empty; the server-side dry runs persist nothing else.
    for overlay in staging production; do
        ns=$(sed -n 's/^NAMESPACE=//p' "$kube/overlays/$overlay/config.env")
        kubectl create namespace "$ns" --dry-run=client -o yaml | kubectl apply -f - >/dev/null
    done
    kubectl apply --dry-run=server -f "$kube/cluster/stunner"
    kubectl apply --dry-run=server -f "$manifests/staging.yaml" -f "$manifests/production.yaml" \
        -f "$manifests/live-packager-staging.yaml" -f "$manifests/live-packager-production.yaml"
fi

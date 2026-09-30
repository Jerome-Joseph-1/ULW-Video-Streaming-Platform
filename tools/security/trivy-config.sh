#!/usr/bin/env bash
# Trivy's misconfiguration checks over what deploy/ ships and runs (docs/adr/0072): the
# Kubernetes manifests and Dockerfiles as they are, and the sandbox's two kustomizations as
# rendered, since their patches are not manifests on their own. The checks are the ones built
# into the pinned Trivy release; --skip-check-update keeps them from being replaced by whatever
# the registry serves today. Any finding, of any severity, not in tools/security/trivyignore.yaml
# (or past its expired_at there) fails.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
trivy=$(tools/security/tools.sh trivy)/trivy
kubectl=$(deploy/local/tools.sh)/kubectl
python3 tools/security/check-allowlists.py

# The two kustomizations are rendered and split one resource to a file, so the allowlist can
# name a single resource (a path is all it can scope by). Their raw directories are left out:
# the patches are fragments, and every resource they list is in the rendering.
scan=build/security/trivy-config
rm -rf "$scan"
mkdir -p "$scan/deploy"
# Trivy takes one directory, so what it checks is copied into one, at the same paths.
git ls-files -z deploy | grep -zv '^deploy/local/cluster/\|^deploy/stunner/' \
    | xargs -0 cp --parents -t "$scan"
for kustomization in deploy/local/cluster deploy/stunner; do
    out=$scan/rendered/${kustomization//\//-}
    mkdir -p "$out"
    "$kubectl" kustomize --load-restrictor LoadRestrictionsNone "$kustomization" \
        | python3 -c '
import sys
import yaml

for doc in yaml.safe_load_all(sys.stdin):
    if not doc:
        continue
    meta = doc["metadata"]
    name = "-".join([doc["kind"], meta.get("namespace", "cluster"), meta["name"]]).lower()
    with open(f"{sys.argv[1]}/{name}.yaml", "x", encoding="utf-8") as f:
        yaml.safe_dump(doc, f, sort_keys=False)
' "$out"
done

TRIVY_CACHE_DIR=${TRIVY_CACHE_DIR:-$root/build/security/trivy-cache} "$trivy" config \
    --skip-check-update --quiet --exit-code 1 \
    --config-data tools/security/trivy-data --ignorefile tools/security/trivyignore.yaml \
    "$@" "$scan"

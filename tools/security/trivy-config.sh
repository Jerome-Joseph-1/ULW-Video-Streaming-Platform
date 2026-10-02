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
    "$kubectl" kustomize --load-restrictor LoadRestrictionsNone "$kustomization" |
        python3 tools/security/split-resources.py "$scan/rendered/${kustomization//\//-}"
done
# A file holding several resources becomes a directory of the same name, less .yaml, with one
# file per resource, so an ignore entry can name one of them (split-resources.py).
while IFS= read -r -d '' manifest; do
    if [[ $(grep -c '^kind:' "$manifest") -gt 1 ]]; then
        python3 tools/security/split-resources.py "${manifest%.yaml}" "$manifest"
        rm "$manifest"
    fi
done < <(find "$scan/deploy" -name '*.yaml' -print0)

TRIVY_CACHE_DIR=${TRIVY_CACHE_DIR:-$root/build/security/trivy-cache} "$trivy" config \
    --skip-check-update --quiet --exit-code 1 \
    --config-data tools/security/trivy-data --ignorefile tools/security/trivyignore.yaml \
    "$@" "$scan"

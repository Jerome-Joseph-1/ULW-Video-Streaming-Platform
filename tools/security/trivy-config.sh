#!/usr/bin/env bash
# Trivy's misconfiguration checks over what deploy/ ships and runs (docs/adr/0072): the Kubernetes
# manifests and Dockerfiles as they are, and the example overlays and the sandbox's two
# kustomizations as rendered, since their patches and config are not manifests on their own. The
# checks are the ones built into the pinned Trivy release; --skip-check-update keeps them from being
# replaced by whatever the registry serves today. Any finding, of any severity, not in
# tools/security/trivyignore.yaml (or past its expired_at there) fails.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
trivy=$(tools/security/tools.sh trivy)/trivy
kubectl=$(deploy/local/tools.sh)/kubectl
python3 tools/security/check-allowlists.py

# The kustomizations are rendered and split one resource to a file, so the allowlist can name a
# single resource (a path is all it can scope by). Their raw directories are left out: the
# patches are fragments, and every resource they list is in the rendering. deploy/kubernetes's
# base is also scanned as written, as an operator may apply its own overlay of it.
scan=build/security/trivy-config
rm -rf "$scan"
mkdir -p "$scan/deploy"
# Trivy takes one directory, so what it checks is copied into one, at the same paths.
git ls-files -z deploy |
    grep -zv '^deploy/local/cluster/\|^deploy/stunner/\|^deploy/kubernetes/overlays/' \
    | xargs -0 cp --parents -t "$scan"
for kustomization in deploy/kubernetes/overlays/staging deploy/kubernetes/overlays/production \
    deploy/local/cluster deploy/stunner; do
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

# ResourceQuotas are scanned on their own. KSV-0040 selects ResourceQuota inputs, but Trivy
# turns a check on for a whole scan once any input matches its selector and then runs it on
# every resource, whose missing spec.hard fails it: one quota in the tree would fail every
# other file with a finding about quotas that is false for all of them. Scanned apart, each
# quota is held to KSV-0040 (hard CPU and memory requests and limits) and to every other check,
# and the rest of the tree to every check KSV-0040 never applied to.
quotas=()
while IFS= read -r -d '' manifest; do
    quotas+=("${manifest#"$scan"/}")
done < <(grep -rlZ --include='*.yaml' '^kind: ResourceQuota$' "$scan" || true)
run_trivy() {
    TRIVY_CACHE_DIR=${TRIVY_CACHE_DIR:-$root/build/security/trivy-cache} "$trivy" config \
        --skip-check-update --quiet --exit-code 1 \
        --config-data "$root/tools/security/trivy-data" \
        --ignorefile "$root/tools/security/trivyignore.yaml" \
        "$@"
}
skips=()
for quota in "${quotas[@]}"; do
    skips+=(--skip-files "$quota")
done
status=0
(cd "$scan" && run_trivy "${skips[@]}" "$@" .) || status=1
for quota in "${quotas[@]}"; do
    (cd "$scan" && run_trivy "$@" "$quota") || status=1
done
exit "$status"

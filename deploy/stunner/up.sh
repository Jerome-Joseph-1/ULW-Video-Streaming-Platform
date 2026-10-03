#!/usr/bin/env bash
# Installs STUNner v1.2.1 and a LiveKit server behind it into the sandbox cluster, creating the
# cluster first if e2e-up.sh has not (kind.yaml, with the outside network of sandbox.sh). Only
# the sandbox: every kubectl call goes through sandbox.sh, which names its kubeconfig and
# context, and nothing is applied before require_sandbox has passed. Rerunning is harmless.
#
# Waits, each with a deadline, until the operator is Available, the GatewayClass Accepted, the
# Gateway Programmed, the UDPRoute Accepted with its backend resolved, and the stunnerd and
# LiveKit pods Available. tests/cluster/stunner_check.py then checks it from the outside.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/deploy/local
tools=$("$here/tools.sh")
# shellcheck source=deploy/local/sandbox.sh
source "$here/sandbox.sh"
# shellcheck source=deploy/local/images.sh
source "$here/images.sh"

# The sandbox's fake credentials. The TURN secret signs the time-windowed credentials the
# checks mint (ADR-0037); the LiveKit pair is the one the local call suite signs tickets with.
turn_secret=ulw-sandbox-turn-testtest123
livekit_keys='ulw-dev-key: ulw-dev-secret-testtest123-not-a-real-secret'

log() { echo "stunner: $*" >&2; }

# The guard reads the kubeconfig kind writes, so the cluster comes first; creating it touches
# only this machine, and nothing reaches kubectl before the guard has passed.
create_cluster
require_sandbox

pinned "$stunner_operator_image" "$stunner_operator_digest"
pinned "$stunnerd_image" "$stunnerd_digest"
pinned "$livekit_image" "$livekit_digest"

# STUNner needs the Gateway API CRDs; they come from Envoy Gateway's pinned install.yaml, the
# release e2e-up.sh installs, so the two never disagree about their version. Only the CRDs: the
# rest of that file runs Envoy Gateway, which is e2e-up.sh's to start.
log "installing the Gateway API and STUNner CRDs"
gateway_api_crds() {
    python3 - "$tools/envoy-gateway.yaml" <<'EOF'
import sys
import yaml

with open(sys.argv[1], encoding="utf-8") as f:
    crds = [d for d in yaml.safe_load_all(f)
            if d and d.get("kind") == "CustomResourceDefinition"
            and d["spec"]["group"] == "gateway.networking.k8s.io"]
yaml.safe_dump_all(crds, sys.stdout)
EOF
}
gateway_api_crds | kubectl apply --server-side --force-conflicts -f - >/dev/null
kubectl apply --server-side --force-conflicts -f "$tools/stunner-crds.yaml" >/dev/null
kubectl wait --for=condition=Established --timeout=60s \
    crd/gatewayclasses.gateway.networking.k8s.io crd/gateways.gateway.networking.k8s.io \
    crd/gatewayconfigs.stunner.l7mp.io crd/dataplanes.stunner.l7mp.io \
    crd/udproutes.stunner.l7mp.io >/dev/null

apply_stdin() { kubectl apply -f - >/dev/null; }
for ns in stunner-system apps-stage; do
    kubectl create namespace "$ns" --dry-run=client -o yaml | apply_stdin
done
# The keys are the ones RUNBOOK.md lists, with sandbox values.
kubectl -n stunner-system create secret generic stunner-secrets --from-literal=type=ephemeral \
    --from-literal="secret=$turn_secret" --dry-run=client -o yaml | apply_stdin
# Chat's call handler signs tickets with the same pair (docs/adr/0086), and names Envoy's
# listener on this host as the URL clients reach LiveKit's /rtc route on.
kubectl -n apps-stage create secret generic sfu-secrets --from-literal=ASKEDIN_ENV=stage \
    --from-literal="LIVEKIT_KEYS=$livekit_keys" --from-literal="TURN_HOST=$outside_gateway" \
    --from-literal="TURN_SECRET=$turn_secret" \
    --from-literal="LIVEKIT_API_KEY=${livekit_keys%%: *}" \
    --from-literal="LIVEKIT_API_SECRET=${livekit_keys#*: }" \
    --from-literal="LIVEKIT_CLIENT_URL=ws://127.0.0.1:18080" --dry-run=client -o yaml | apply_stdin

log "applying the operator, the Gateway and LiveKit"
kubectl kustomize --load-restrictor LoadRestrictionsNone "$root/deploy/stunner" | apply_stdin

kubectl -n stunner-system rollout status deployment/stunner-gateway-operator-controller-manager \
    --timeout=180s
kubectl wait --for=condition=Accepted --timeout=120s gatewayclass/stunner-gatewayclass
kubectl -n apps-stage wait --for=condition=Programmed --timeout=120s gateway/stunner
for condition in Accepted ResolvedRefs; do
    kubectl -n apps-stage wait --timeout=120s udproutes.stunner.l7mp.io/livekit \
        --for="jsonpath={.status.parents[0].conditions[?(@.type==\"$condition\")].status}=True"
done
# The operator creates the stunnerd Deployment, under the Gateway's name, once it has rendered
# the Gateway's configuration.
kubectl -n apps-stage wait --for=create --timeout=120s deployment/stunner
kubectl -n apps-stage rollout status deployment/stunner --timeout=180s
# A changed secret reaches LiveKit only through a new pod.
kubectl -n apps-stage rollout restart deployment/livekit >/dev/null
kubectl -n apps-stage rollout status deployment/livekit --timeout=180s
# Chat (when e2e-up.sh deployed it) reads sfu-secrets only at start: until its pods restart they
# answer calls with calls_disabled.
if kubectl -n apps-stage get deployment chat >/dev/null 2>&1; then
    kubectl -n apps-stage rollout restart deployment/chat >/dev/null
    kubectl -n apps-stage rollout status deployment/chat --timeout=300s
fi
log "up: TURN on $outside_gateway:3478/udp, LiveKit signalling on http://$outside_gateway:17880"

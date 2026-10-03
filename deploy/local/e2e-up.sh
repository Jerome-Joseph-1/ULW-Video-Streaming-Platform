#!/usr/bin/env bash
# Brings up a sandbox operator's cluster on this machine, from scratch: a kind cluster, Postgres
# and MinIO beside it (as an operator's often run outside the cluster), Envoy Gateway, the mock
# auth-service, and deploy/kubernetes's video-gateway, video-worker and chat, configured by
# config.env and built from this checkout (and the live packager's image, which runs no pod here), and STUNner with
# LiveKit behind it (deploy/stunner/up.sh). Rerunning rebuilds the images and reapplies
# everything; e2e-down.sh removes it.
# Nothing here knows how to reach any real operator's infrastructure.
#
#   ULW_BUILDER    build with this docker buildx builder instead of the default one; the images
#                  then reach the node as OCI archives and never enter the local image store
#                  Behind a proxy on the host's loopback the builder needs host networking:
#                    docker buildx create --name B --driver docker-container \
#                      --driver-opt image=moby/buildkit@sha256:<pinned digest> \
#                      --driver-opt network=host --driver-opt env.HTTPS_PROXY=$HTTPS_PROXY \
#                      --driver-opt env.SSL_CERT_FILE=/proxy-ca.crt --buildkitd-config F
#                  where F holds [worker.oci] networkMode = "host" (so the RUN steps reach the
#                  proxy too). buildkitd reads the proxy's CA from that path, so once the
#                  builder exists (docker buildx inspect B --bootstrap), copy the file in and
#                  restart it: docker cp <ca bundle> buildx_buildkit_B0:/proxy-ca.crt, then
#                  docker restart buildx_buildkit_B0. Its build cache is several GB:
#                  `docker buildx prune -a --builder B` once the images are loaded.
#   ULW_CA_BUNDLE  CA bundle handed to image builds (build secret ca-bundle) on machines whose
#                  outbound HTTPS goes through a TLS-inspecting proxy
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/deploy/local
state=$here/.state
tools=$("$here/tools.sh")
# shellcheck source=deploy/local/sandbox.sh
source "$here/sandbox.sh"
kind() { "$tools/kind" "$@"; }

# shellcheck source=deploy/local/images.sh
source "$here/images.sh"
pg=ulw-e2e-pg
minio=ulw-e2e-minio
# Throwaway credentials; they guard containers reachable only from this machine.
password=testtest123
bucket=ulw-e2e                        # config.env's BUCKET

log() { echo "e2e-up: $*" >&2; }

mkdir -p "$state/images" "$state/pki"

create_cluster
require_sandbox

# The worker's seccomp profile, where deploy/kubernetes/RUNBOOK.md (step 2) installs it.
node=$cluster-control-plane
docker exec "$node" mkdir -p /var/lib/kubelet/seccomp/profiles
docker cp "$root/deploy/kubernetes/cluster/seccomp/ulw-worker.json" \
    "$node:/var/lib/kubelet/seccomp/profiles/ulw-worker.json"

# On a host with DMI (any cloud VM, the CI runners among them) kind's entrypoint bind-mounts
# fake product_name and product_uuid files over the node's sysfs, so nodes of one cluster get
# distinct ids. The kernel mounts a fresh sysfs in a user namespace only while every sysfs
# mount it could reveal is fully visible (fs/namespace.c, mount_too_revealing), and a file
# mounted over one hides it: runc then fails every hostUsers: false pod sandbox with "error
# mounting sysfs ... operation not permitted", and the worker never starts. One node needs no
# distinct id; the entrypoint puts them back whenever the node restarts, and this runs again.
# shellcheck disable=SC2016 # expanded by the node's shell
docker exec "$node" sh -c '
    for f in /sys/devices/virtual/dmi/id/product_name /sys/devices/virtual/dmi/id/product_uuid; do
        while findmnt --mountpoint "$f" >/dev/null; do umount "$f"; done
    done'

build_args=(--build-arg "ULW_GIT_SHA=$(git -C "$root" rev-parse --short=12 HEAD)")
for proxy in HTTPS_PROXY https_proxy NO_PROXY no_proxy; do
    [[ -n ${!proxy:-} ]] && build_args+=(--build-arg "$proxy")
done
[[ -n ${ULW_CA_BUNDLE:-} ]] && build_args+=(--secret "id=ca-bundle,src=$ULW_CA_BUNDLE")

# build IMAGE CONTEXT DOCKERFILE [TARGET]
build() {
    local image=$1 context=$2 dockerfile=$3 target=${4:-}
    local args=("${build_args[@]}" --file "$dockerfile" ${target:+--target "$target"})
    log "building $image"
    if [[ -n ${ULW_BUILDER:-} ]]; then
        local archive=$state/images/${image//[\/:]/_}.tar
        docker buildx build --builder "$ULW_BUILDER" "${args[@]}" \
            --output "type=oci,dest=$archive,name=$image" "$context"
        kind load image-archive --name "$cluster" "$archive"
        rm -f "$archive"
    else
        docker buildx build "${args[@]}" --load --tag "$image" "$context"
        load_image "$image"
    fi
}

build "${built_images[0]}" "$root" "$root/deploy/docker/Dockerfile" gateway
build "${built_images[1]}" "$root" "$root/deploy/docker/Dockerfile" worker
build "${built_images[2]}" "$here/mock-auth" "$here/mock-auth/Dockerfile"
build "${built_images[3]}" "$root" "$root/deploy/docker/Dockerfile" chat
build "${built_images[4]}" "$root" "$root/deploy/docker/Dockerfile" live-packager

pinned "$eg_image" "$eg_digest"
pinned "$envoy_image" "$envoy_digest"
pinned "$kube_router_image" "$kube_router_digest"

# start NAME DOCKER_RUN_ARGS... runs a backing service on the kind network, where the node
# reaches it as an operator's cluster reaches services outside it; its data is a tmpfs and goes with it.
start() {
    local name=$1
    shift
    if [[ -z $(docker ps --quiet --filter "name=^$name$") ]]; then
        docker rm --force "$name" >/dev/null 2>&1 || true
        log "starting $name"
        docker run --detach --name "$name" --network kind "$@" >/dev/null
    fi
}
start "$pg" --tmpfs /var/lib/postgresql/data --env "POSTGRES_PASSWORD=$password" "$pg_image"
# MinIO is also published on the host's loopback: presigned segment URLs name it as the pods
# do (minio:9000), and the playback check reaches it there with that Host header, as a viewer
# resolving the store's name would. CORS as in compose.yaml (ADR-0028). Incomplete multipart
# uploads are aborted after 7 days, as R2's lifecycle rule does (RUNBOOK section 3); MinIO
# refuses AbortIncompleteMultipartUpload in a bucket lifecycle and has this server-wide setting
# instead, whose default of 24 hours would cut short uploads the gateway allows 6 days.
start "$minio" --tmpfs /data --env MINIO_ROOT_USER=ulw-e2e --env "MINIO_ROOT_PASSWORD=$password" \
    --env 'MINIO_API_CORS_ALLOW_ORIGIN=*' --env MINIO_API_STALE_UPLOADS_EXPIRY=168h \
    --publish 127.0.0.1:19000:9000 "$minio_image" server /data

until docker exec "$pg" pg_isready --quiet --username postgres; do sleep 1; done
until docker exec "$minio" mc alias set local http://127.0.0.1:9000 ulw-e2e "$password" \
    >/dev/null 2>&1; do sleep 1; done
docker exec "$minio" mc mb --ignore-existing "local/$bucket" >/dev/null
# An environment override is not stored: `config get` still shows stale_uploads_expiry=24h and
# reports the override on a comment line of its own, which is the only place the effective value
# shows (mc has no command that prints it).
docker exec "$minio" mc admin config get local api | grep -qxF '# MINIO_API_STALE_UPLOADS_EXPIRY=168h' \
    || { log "MinIO did not take the 7-day stale upload expiry"; exit 1; }

# The mock auth-service's certificate, from a CA that exists only here. The gateway trusts
# this CA (gateway-patch.yaml) exactly as it trusts the public ones in production.
pki=$state/pki
if [[ ! -f $pki/mock-auth.crt ]]; then
    log "issuing the sandbox CA and mock-auth certificate"
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 30 \
        -subj "/CN=ulw sandbox CA" -keyout "$pki/ca.key" -out "$pki/ca.crt" 2>/dev/null
    openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -subj "/CN=mock-auth" \
        -keyout "$pki/mock-auth.key" -out "$pki/mock-auth.csr" 2>/dev/null
    openssl x509 -req -in "$pki/mock-auth.csr" -CA "$pki/ca.crt" -CAkey "$pki/ca.key" \
        -CAcreateserial -days 30 -out "$pki/mock-auth.crt" \
        -extfile <(printf 'subjectAltName=DNS:mock-auth.auth.svc.cluster.local,DNS:mock-auth.auth.svc\n') \
        2>/dev/null
fi

log "installing Envoy Gateway v1.9.2"
kubectl apply --server-side --force-conflicts -f "$tools/envoy-gateway.yaml" >/dev/null
kubectl -n envoy-gateway-system rollout status deployment/envoy-gateway --timeout=180s

apply_stdin() { kubectl apply -f - >/dev/null; }
for ns in gateway-system ulw auth; do
    kubectl create namespace "$ns" --dry-run=client -o yaml | apply_stdin
done
kubectl -n ulw create configmap sandbox-ca --from-file=ca.crt="$pki/ca.crt" \
    --dry-run=client -o yaml | apply_stdin
kubectl -n auth create secret tls mock-auth-tls --cert="$pki/mock-auth.crt" \
    --key="$pki/mock-auth.key" --dry-run=client -o yaml | apply_stdin
# The keys deploy/kubernetes/RUNBOOK.md (step 3) lists, with sandbox values; everything else
# comes from config.env.
for service in video-gateway video-worker; do
    kubectl -n ulw create secret generic "$service-secrets" \
        --from-literal="ULW_DATABASE_URL=postgresql://postgres:$password@postgres:5432/postgres" \
        --from-literal=ULW_S3_ACCESS_KEY_ID=ulw-e2e \
        --from-literal="ULW_S3_SECRET_ACCESS_KEY=$password" \
        --dry-run=client -o yaml | apply_stdin
done
# Chat's, with a node secret made for this run.
kubectl -n ulw create secret generic chat-secrets \
    --from-literal="ULW_DATABASE_URL=postgresql://postgres:$password@postgres:5432/postgres" \
    --from-literal="ULW_NODE_SECRET=$(openssl rand -base64 48)" --dry-run=client -o yaml |
    apply_stdin

log "applying the sandbox and deploy/kubernetes/base"
kubectl kustomize --load-restrictor LoadRestrictionsNone "$here/cluster" | apply_stdin

# endpoint NAME PORT_NAME PORT CONTAINER points Service NAME at a backing container.
endpoint() {
    local address
    address=$(docker inspect --format '{{(index .NetworkSettings.Networks "kind").IPAddress}}' "$4")
    apply_stdin <<EOF
apiVersion: discovery.k8s.io/v1
kind: EndpointSlice
metadata:
  name: $1
  namespace: ulw
  labels:
    kubernetes.io/service-name: $1
addressType: IPv4
ports:
  - name: $2
    port: $3
endpoints:
  - addresses: ["$address"]
EOF
}
endpoint postgres postgres 5432 "$pg"
endpoint minio s3 9000 "$minio"

# The images are rebuilt on every run under the same tag, so pods from an earlier run would
# keep the old ones.
kubectl -n ulw rollout restart deployment/video-gateway deployment/video-worker \
    deployment/chat >/dev/null
kubectl -n auth rollout status deployment/mock-auth --timeout=120s
kubectl -n ulw rollout status deployment/video-gateway --timeout=300s
kubectl -n ulw rollout status deployment/video-worker --timeout=300s
# Chat's tables come from the gateway's init container, so a chat pod that started before it
# had migrated waits unready (or restarts) until they exist.
kubectl -n ulw rollout status deployment/chat --timeout=300s
kubectl -n envoy-gateway-system wait --for=condition=Available deployment \
    --selector=gateway.envoyproxy.io/owning-gateway-name=public-gateway --timeout=180s
# Envoy takes a moment to program the routes after its pod is ready.
curl -fsS -o /dev/null --retry 60 --retry-all-errors --retry-delay 1 \
    -X POST "http://127.0.0.1:18080/mock-auth/token?sub=probe"
# The realtime plane's way in: STUNner and the LiveKit server behind it.
"$root/deploy/stunner/up.sh"
log "up: http://127.0.0.1:18080 (kubeconfig $kubeconfig, context $context)"

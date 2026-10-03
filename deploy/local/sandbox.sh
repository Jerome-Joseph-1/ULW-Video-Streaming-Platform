# The sandbox cluster's name, kubeconfig and context, and the helpers that create it and load
# images into it; sourced by the scripts beside it and deploy/stunner/up.sh after they set
# $here (deploy/local) and $tools. Every kubectl call they make goes through kubectl() below,
# which names the kubeconfig and context outright: neither a KUBECONFIG in the caller's
# environment nor a missing file can point one at another cluster.
cluster=ulw-e2e
kubeconfig=$here/.state/kubeconfig
context=kind-$cluster

kubectl() { "$tools/kubectl" --kubeconfig "$kubeconfig" --context "$context" "$@"; }

# The sandbox's stand-in for the internet: a Docker network the kind network cannot see.
# kind.yaml publishes the node's WebRTC-facing ports on its gateway address, the host's side of
# that network, so a client in it reaches the node through the same DNAT a client on the
# internet goes through to reach a cluster's node, and keeps its own source address the whole way
# (tests/cluster/stunner_check.py). The network is in Docker's routed mode (Docker 27 or later):
# in the default NAT mode Docker masquerades the client as the host, and with masquerading
# merely switched off it still drops every packet addressed to a container that arrives from
# another bridge, the replies included. Docker before 29 also needs its userland proxy off and
# DNATed packets from this network accepted in DOCKER-USER, or the client reaches the node
# through docker-proxy, from the kind network's gateway address; e2e.yml sets both on its
# runner. 198.18.0.0/15 is reserved for benchmarking (RFC 2544) and never routed, so it
# collides with no network a host is on.
outside_network=$cluster-outside
outside_subnet=198.18.0.0/24
outside_gateway=198.18.0.1

# Creates the kind cluster unless it exists, and the outside network before it: kind.yaml binds
# ports to the network's gateway address, which must exist when the node starts.
create_cluster() {
    if [[ -z $(docker network ls --quiet --filter "name=^$outside_network$") ]]; then
        docker network create --subnet "$outside_subnet" --gateway "$outside_gateway" \
            --opt com.docker.network.bridge.gateway_mode_ipv4=routed "$outside_network" \
            >/dev/null
    fi
    if ! "$tools/kind" get clusters | grep -qx "$cluster"; then
        echo "creating kind cluster $cluster" >&2
        "$tools/kind" create cluster --name "$cluster" --config "$here/kind.yaml" \
            --kubeconfig "$kubeconfig" --wait 180s
    fi
    # Rewritten on every run, so the file always describes this cluster, whatever became of it.
    "$tools/kind" export kubeconfig --name "$cluster" --kubeconfig "$kubeconfig"
}

# Fails unless the kubeconfig exists and its context is a kind cluster on this machine's
# loopback, which is where kind binds every API server it creates.
require_sandbox() {
    if [[ ! -f $kubeconfig ]]; then
        echo "$kubeconfig is missing; run make e2e-up first" >&2
        return 1
    fi
    local server
    server=$("$tools/kubectl" --kubeconfig "$kubeconfig" config view --minify \
        --context "$context" -o jsonpath='{.clusters[0].cluster.server}')
    if [[ $server != https://127.0.0.1:* ]]; then
        echo "context $context in $kubeconfig points at '$server', not a kind cluster on" \
            "this machine; refusing" >&2
        return 1
    fi
}

# load_image IMAGE puts an image from the local store into the node. Only the node's platform
# is saved: the local store may hold just that one of a multi-platform image.
load_image() {
    local archive=$here/.state/images/${1//[\/:]/_}.tar
    mkdir -p "$here/.state/images"
    docker save --platform linux/amd64 --output "$archive" "$1"
    "$tools/kind" load image-archive --name "$cluster" "$archive"
    rm -f "$archive"
}

# pinned IMAGE DIGEST pulls IMAGE's content by digest and loads it into the node under IMAGE's
# tag, the name the manifests use there (images.sh).
pinned() {
    docker pull --quiet "${1%:*}@$2" >/dev/null
    docker tag "${1%:*}@$2" "$1"
    load_image "$1"
}

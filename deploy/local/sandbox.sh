# The sandbox cluster's name, kubeconfig and context, sourced by the scripts beside it after they
# set $here and $tools. Every kubectl call they make goes through kubectl() below, which names
# the kubeconfig and context outright: neither a KUBECONFIG in the caller's environment nor a
# missing file can point one at another cluster.
cluster=ulw-e2e
kubeconfig=$here/.state/kubeconfig
context=kind-$cluster

kubectl() { "$tools/kubectl" --kubeconfig "$kubeconfig" --context "$context" "$@"; }

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

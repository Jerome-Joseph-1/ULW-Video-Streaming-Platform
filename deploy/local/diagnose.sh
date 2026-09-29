#!/usr/bin/env bash
# Collects what a failed sandbox run leaves behind into DIR (default deploy/local/.state/diagnose):
# the cluster's pods, events, and the descriptions and logs of every pod that is not ready, plus
# the host facts that decide whether the worker's user namespaces can exist at all. Prints the
# lines that usually name the cause, so a CI log alone is enough to read them. Never fails: it
# runs after something else already has.
#
#   diagnose.sh [DIR]
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/.state/diagnose}
tools=$("$here/tools.sh")
# shellcheck source=deploy/local/sandbox.sh
source "$here/sandbox.sh"
mkdir -p "$out"

section() { printf '\n===== %s\n' "$*"; }

{
    section uname; uname -a
    section sysctl
    sysctl kernel.apparmor_restrict_unprivileged_userns kernel.unprivileged_userns_clone \
        user.max_user_namespaces 2>&1
    section uid_map; cat /proc/self/uid_map
    section "kernel log (apparmor, seccomp, denials)"
    sudo -n dmesg 2>/dev/null | grep -iE 'apparmor|seccomp|denied|userns' | tail -50
} >"$out/host.txt" 2>&1
docker info >"$out/docker-info.txt" 2>&1
{
    section "docker info (runtime, cgroups, security)"
    grep -iE 'server version|storage driver|cgroup|runtime|security|apparmor|seccomp|userns|rootless|kernel|operating' \
        "$out/docker-info.txt"
} >>"$out/host.txt"

if ! require_sandbox 2>"$out/cluster.txt"; then
    cat "$out/host.txt" "$out/cluster.txt"
    exit 0
fi

kubectl get pods -A -o wide >"$out/pods.txt" 2>&1
kubectl get events -A --sort-by=.lastTimestamp 2>&1 | tail -200 >"$out/events.txt"

# namespace/name of every pod with a container that is not ready, completed Jobs aside.
not_ready=$(kubectl get pods -A -o \
    jsonpath='{range .items[?(@.status.phase!="Succeeded")]}{.metadata.namespace}/{.metadata.name}{" "}{.status.containerStatuses[*].ready}{"\n"}{end}' \
    2>/dev/null | awk '$0 !~ /^[^ ]+ (true ?)+$/ {print $1}')

for pod in $not_ready; do
    ns=${pod%/*} name=${pod#*/}
    file=$out/pod-$ns-$name.txt
    {
        section "describe $pod"; kubectl -n "$ns" describe pod "$name"
        section "logs $pod"; kubectl -n "$ns" logs "$name" --all-containers --tail=200
        section "previous logs $pod"
        kubectl -n "$ns" logs "$name" --all-containers --previous --tail=200
    } >"$file" 2>&1
done

node=$cluster-control-plane
docker exec "$node" journalctl -u kubelet --no-pager 2>&1 | tail -300 >"$out/kubelet.txt"
docker exec "$node" journalctl -u containerd --no-pager 2>&1 | tail -300 >"$out/containerd.txt"

# The job log's copy: the host facts, the pod table, the recent events, and each unready pod's
# state, last events and log tail.
cat "$out/host.txt"
section pods; cat "$out/pods.txt"
section "events (last 60)"; tail -60 "$out/events.txt"
for pod in $not_ready; do
    file=$out/pod-${pod%/*}-${pod#*/}.txt
    section "unready $pod"
    grep -A12 -E '^ *(State|Last State):' "$file" | head -60
    sed -n '/^Events:/,/^=====/p' "$file" | tail -25
    sed -n '/^===== logs /,$p' "$file" | tail -60
done
section "kubelet errors"
grep -iE 'error|fail' "$out/kubelet.txt" | tail -30
section "containerd errors"
grep -iE 'error|fail' "$out/containerd.txt" | tail -30
exit 0

#!/usr/bin/env bash
# Runs at every start of the development container (devcontainer.json).
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)

# The two kernel settings CI's setup action makes (.github/actions/setup/action.yml): the
# worker's sandbox needs unprivileged user namespaces, which Ubuntu's AppArmor policy refuses,
# and the sanitizers' shadow memory needs fewer mmap random bits. The container is privileged
# (docker-in-docker), so it may; on a host that refuses, the affected tests skip or fail and
# tools/dev/doctor.sh says why.
for setting in kernel.apparmor_restrict_unprivileged_userns=0 vm.mmap_rnd_bits=28; do
    sudo sysctl -qw "$setting" 2>/dev/null || echo "post-start: could not set $setting" >&2
done

# The feature's dockerd starts with the container; give it a moment before compose talks to it.
for _ in $(seq 1 30); do
    docker info >/dev/null 2>&1 && break
    sleep 1
done
"$root/tools/dev/services.sh" up || echo "post-start: the local services did not start; run tools/dev/services.sh up" >&2

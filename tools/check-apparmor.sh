#!/usr/bin/env bash
# Runs ulw_sandbox under the worker's AppArmor profile (deploy/kubernetes/cluster/apparmor/
# ulw-worker), on a host that enforces AppArmor and restricts unprivileged user namespaces as
# Ubuntu 24.04 does by default: the sandbox's start-up check, then ffprobe through it. Each must
# succeed under the profile. As a control, the start-up check must fail under a profile that
# denies mounts, as the container runtime's default does, so that a pass here means the profile
# is what lets it through. Needs root (sudo) to load the profiles and set the sysctl, which it
# puts back.
#
#   tools/check-apparmor.sh SANDBOX   # SANDBOX: the built ulw_sandbox
set -euo pipefail

sandbox=$(realpath "$1")
root=$(cd "$(dirname "$0")/.." && pwd)
profile=$root/deploy/kubernetes/cluster/apparmor/ulw-worker
if [[ $(cat /sys/module/apparmor/parameters/enabled 2>/dev/null) != Y ]]; then
    echo "check-apparmor: AppArmor is not enabled on this host" >&2
    exit 1
fi

key=kernel.apparmor_restrict_unprivileged_userns
before=$(sysctl -n "$key" 2>/dev/null || true)
control=$(mktemp)
cleanup() {
    [[ -n $before ]] && sudo sysctl -qw "$key=$before"
    sudo apparmor_parser -R "$control" 2>/dev/null || true
    rm -f "$control"
    sudo rm -rf /var/cache/ulw-worker/apparmor-check
}
trap cleanup EXIT
[[ -n $before ]] && sudo sysctl -qw "$key=1"

sudo apparmor_parser -r -W "$profile"
# The worker's profile without its mount and userns rules: what a node's default gives.
sed -e '/^  userns,$/d' -e '/^  mount /d' -e 's/^  umount,$/  umount,\n  deny mount,/' \
    -e 's/^profile ulw-worker /profile ulw-worker-control /' \
    -e 's/peer=ulw-worker,/peer=ulw-worker-control,/' "$profile" >"$control"
sudo apparmor_parser -r -W "$control"

dir=/var/cache/ulw-worker/apparmor-check
sudo install -d -o "$(id -u)" -g "$(id -g)" "$dir"
run() {
    aa-exec -p "$1" -- "$sandbox" --writable "$dir" --address-space $((2 << 30)) \
        --cpu-seconds 10 "${@:2}"
}

failures=0
if run ulw-worker; then
    echo "ok: the sandbox starts under ulw-worker"
else
    echo "FAIL: the sandbox does not start under ulw-worker (exit $?)"
    failures=$((failures + 1))
fi
if run ulw-worker -- ffprobe -version >/dev/null; then
    echo "ok: ffprobe runs in the sandbox under ulw-worker"
else
    echo "FAIL: ffprobe does not run in the sandbox under ulw-worker (exit $?)"
    failures=$((failures + 1))
fi
if run ulw-worker-control 2>/dev/null; then
    echo "FAIL: the sandbox starts without the profile's rules too; this check proves nothing"
    failures=$((failures + 1))
else
    echo "ok: without the profile's mount and userns rules the sandbox is refused"
fi
exit "$failures"

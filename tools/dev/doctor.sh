#!/usr/bin/env bash
# Checks that this machine can build, test and run the platform, and says what to do about
# each thing that is missing. Exit status 0 when nothing is.
#
#   tools/dev/doctor.sh
set -uo pipefail
# shellcheck source=tools/dev/lib.sh
. "$(dirname "$0")/lib.sh"

problems=0
ok() { printf '  ok    %s\n' "$1"; }
warn() { printf '  warn  %s\n        %s\n' "$1" "$2"; }
bad() {
    printf '  FAIL  %s\n        %s\n' "$1" "$2"
    problems=$((problems + 1))
}
have() { command -v "$1" >/dev/null 2>&1; }

echo "toolchain"
for tool in gcc-14 g++-14 clang-19 cmake ninja ccache jq pkg-config psql ffmpeg node npm go cargo; do
    if have "$tool"; then ok "$tool"; else bad "$tool" "not on PATH; rebuild the dev container (or see README.md, Build and test)"; fi
done

echo "kernel"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if cc -O2 "$dev_root/tools/io_uring_probe.c" -o "$work/probe" -luring 2>/dev/null &&
    "$work/probe" >"$work/probe.out" 2>&1; then
    ok "io_uring: every feature the primary reactor needs"
else
    warn "io_uring: not usable here (tools/io_uring_probe.c says what is missing)" \
        "the services fall back to epoll with ULW_REACTOR=epoll; the io_uring tests fail"
fi
if unshare --user --map-root-user --net true 2>/dev/null; then
    ok "unprivileged user namespaces (the worker's ffmpeg sandbox)"
else
    bad "unprivileged user namespaces are refused" \
        "the worker will not start; sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0"
fi
memlock=$(ulimit -l)
if [[ $memlock == unlimited ]]; then
    ok "RLIMIT_MEMLOCK unlimited"
else
    warn "RLIMIT_MEMLOCK is $memlock KiB" \
        "ctest's parallel io_uring tests may fail; sudo prlimit --pid \$\$ --memlock=unlimited:unlimited"
fi

echo "docker"
if have docker && docker info >/dev/null 2>&1; then
    ok "docker daemon"
    for container in ulw-pg ulw-minio; do
        state=$(docker inspect -f '{{.State.Health.Status}}' "$container" 2>/dev/null || echo absent)
        if [[ $state == healthy ]]; then ok "$container healthy"; else bad "$container: $state" "tools/dev/services.sh up"; fi
    done
else
    bad "no docker daemon" "the local Postgres and MinIO need it; in a codespace, rebuild the container"
fi

echo "build"
for target in apps/gateway/gateway_server apps/worker/transcode_worker apps/chat/chat_server \
    apps/migrate/ulw_migrate tools/devtoken/ulw_devtoken; do
    if [[ -x $dev_build/$target ]]; then ok "$target"; else warn "$target not built in ${dev_build#"$dev_root"/}" "tools/dev/build.sh"; fi
done

echo "cloud services (used with ULW_DEV_BACKEND=cloud; docs/development/cloud-dev.md)"
check_cloud() {
    local what=$1 missing=() name
    shift
    for name in "$@"; do [[ -n ${!name:-} ]] || missing+=("$name"); done
    if ((${#missing[@]})); then
        warn "$what: not configured" "missing ${missing[*]}"
        return 1
    fi
    ok "$what: configured"
}
if check_cloud "Postgres (Neon)" ULW_CLOUD_DATABASE_URL; then
    if psql -X -At -d "$ULW_CLOUD_DATABASE_URL" -c 'SELECT 1' >/dev/null 2>"$work/psql.err"; then
        ok "Postgres (Neon): reachable"
    else
        bad "Postgres (Neon): cannot connect" "$(head -n 1 "$work/psql.err")"
    fi
fi
if check_cloud "object store (R2)" ULW_CLOUD_R2_ACCOUNT_ID ULW_CLOUD_R2_BUCKET \
    ULW_CLOUD_R2_ACCESS_KEY_ID ULW_CLOUD_R2_SECRET_ACCESS_KEY; then
    status=$(curl -sS -o /dev/null -w '%{http_code}' --aws-sigv4 aws:amz:auto:s3 \
        --user "$ULW_CLOUD_R2_ACCESS_KEY_ID:$ULW_CLOUD_R2_SECRET_ACCESS_KEY" \
        "https://$ULW_CLOUD_R2_ACCOUNT_ID.r2.cloudflarestorage.com/$ULW_CLOUD_R2_BUCKET?list-type=2&max-keys=1" 2>/dev/null)
    if [[ $status == 200 ]]; then ok "object store (R2): bucket readable"; else bad "object store (R2): listing the bucket answered $status" "check the account id, bucket name and the token's permissions"; fi
fi
check_cloud "LiveKit Cloud" ULW_CLOUD_LIVEKIT_URL ULW_CLOUD_LIVEKIT_API_KEY ULW_CLOUD_LIVEKIT_API_SECRET || true

echo
if ((problems)); then
    echo "doctor: $problems problem(s)"
    exit 1
fi
echo "doctor: nothing missing"

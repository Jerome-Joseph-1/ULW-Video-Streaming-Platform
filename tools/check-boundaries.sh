#!/usr/bin/env bash
# Fails when a module reaches across a layer it must not depend on.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

status=0
fail() {
    echo "boundary violation: $1" >&2
    echo "$2" >&2
    status=1
}

check() {
    local flags=-rnE
    if [[ $1 == -i ]]; then
        flags=-rniE
        shift
    fi
    local what=$1 pattern=$2
    shift 2
    local hits
    hits=$(grep "$flags" --include='*.hpp' --include='*.h' --include='*.cpp' --include='*.cc' \
        "$pattern" "$@" 2>/dev/null || true)
    [[ -n $hits ]] && fail "$what" "$hits"
    return 0
}

check "core includes a platform or vendor header" \
    '#include <(unistd|sys/|netinet/|arpa/|openssl/|curl/|libpq|libav|liburing|linux/)' core
check "cross-module relative include" \
    '#include "\.\./\.\.' core os net http codec rt infra apps tests
# Anchored at a word or '_' boundary so that identifiers such as DeviceTag do not read as "etag".
check -i "vendor vocabulary in a core header" \
    'part_number|multipart|(\b|_)(etag|pg_|s3(\b|_))' core/include

if [[ -d codec ]]; then
    check "codec depends on rt/infra/apps" '#include "(rt|infra|apps)/' codec
fi

# The realtime runtime defines the ports its adapters implement, never the other way round.
if [[ -d rt ]]; then
    check "rt depends on infra/apps" '#include "(infra|apps)/' rt
fi

if [[ -f apps/gateway/CMakeLists.txt ]]; then
    hits=$(grep -nE '^[^#]*(ffmpeg|libav)' apps/gateway/CMakeLists.txt || true)
    [[ -n $hits ]] && fail "gateway links ffmpeg/libav" "$hits"
    hits=$(grep -nE '^[^#]*live[-_]packager' apps/gateway/CMakeLists.txt || true)
    [[ -n $hits ]] && fail "gateway links the live packager" "$hits"
    check "gateway includes the live packager" '#include "(live_packager|media_playlist|live_window)' apps/gateway
fi

if [[ -d net ]]; then
    check "net depends on http" '#include "http/' net
fi
if [[ -d http ]]; then
    check "http reaches into net internals" '#include "net/(src|detail)/' http
fi
if [[ -d apps/worker ]]; then
    check "worker depends on net/http" '#include "(net|http)/' apps/worker
fi
if [[ -d apps/live-packager ]]; then
    check "live packager depends on net/http/rt" '#include "(net|http|rt)/' apps/live-packager
fi

exit $status

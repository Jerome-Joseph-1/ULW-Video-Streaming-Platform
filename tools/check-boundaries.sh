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
    local what=$1 pattern=$2
    shift 2
    local hits
    hits=$(grep -rnE --include='*.hpp' --include='*.h' --include='*.cpp' --include='*.cc' \
        "$pattern" "$@" 2>/dev/null || true)
    [[ -n $hits ]] && fail "$what" "$hits"
    return 0
}

check "core includes a platform or vendor header" \
    '#include <(unistd|sys/|netinet/|arpa/|openssl/|curl/|libpq|libav|liburing|linux/)' core
check "cross-module relative include" \
    '#include "\.\./\.\.' core net http codec rt infra apps tests
check "vendor vocabulary in a core header" \
    'part_number|ETag|etag|s3_|multipart' core/include

if [[ -d codec ]]; then
    check "codec depends on rt/infra/apps" '#include "(rt|infra|apps)/' codec
fi

if [[ -f apps/gateway/CMakeLists.txt ]]; then
    hits=$(grep -nE '^[^#]*(ffmpeg|libav)' apps/gateway/CMakeLists.txt || true)
    [[ -n $hits ]] && fail "gateway links ffmpeg/libav" "$hits"
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

exit $status

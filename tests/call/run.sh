#!/usr/bin/env bash
# The call suite against a build tree, in one command:
#   tests/call/run.sh [build-dir, default build/ci] [playwright test arguments...]
# Needs Node 20 or later. The browser is the pinned Chrome for Testing that tests/e2e uses,
# fetched by fetch-chrome.sh.
#
# By default both peers run on this host against the LiveKit server of deploy/local/compose.yaml,
# which is started if it is not running (and left running). The environment can point it
# elsewhere:
#   LIVEKIT_API_URL, LIVEKIT_CLIENT_URL, LIVEKIT_API_KEY, LIVEKIT_API_SECRET
#       Another LiveKit, such as the one deploy/stunner/up.sh puts behind STUNner in the kind
#       sandbox. The local server is then left alone.
#   ULW_CALL_OUTSIDE_CONTAINER=<name>
#       Runs the second peer of the call inside that container's network namespace (a sleeper on
#       the sandbox's outside network, ulw-e2e-outside), so that it reaches the SFU as a client
#       on the internet would, and requires its media to flow through a TURN relay. Its page
#       comes from the page server on ULW_CALL_PAGE_PORT (default 48123), reached through a
#       loopback forwarder in that namespace. Needs root, for nsenter.
# libcurl (the harness) and Node honour the proxy variables: an SFU on the outside network
# (198.18.0.0/24) must be in no_proxy, which outside mode adds.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build=$(realpath "${1:-$root/build/ci}")
shift $(($# > 0 ? 1 : 0))

export ULW_CALL_HARNESS=$build/tests/ulw_call_harness
if [[ ! -x $ULW_CALL_HARNESS ]]; then
    echo "run.sh: $ULW_CALL_HARNESS missing; build the ulw_call_harness target" >&2
    exit 1
fi

local_livekit=http://127.0.0.1:7880
# The fake key pair of deploy/local/compose.yaml, which the sandbox's LiveKit shares.
export LIVEKIT_API_KEY=${LIVEKIT_API_KEY:-ulw-dev-key}
export LIVEKIT_API_SECRET=${LIVEKIT_API_SECRET:-ulw-dev-secret-testtest123-not-a-real-secret}
export LIVEKIT_API_URL=${LIVEKIT_API_URL:-$local_livekit}
export LIVEKIT_CLIENT_URL=${LIVEKIT_CLIENT_URL:-ws://127.0.0.1:7880}
if [[ $LIVEKIT_API_URL == "$local_livekit" ]]; then
    docker compose -f "$root/deploy/local/compose.yaml" --profile calls up -d --wait livekit \
        >/dev/null
fi

ULW_E2E_CHROME=${ULW_E2E_CHROME:-$("$here/fetch-chrome.sh")}
export ULW_E2E_CHROME

forwarder=
wrapper=
cleanup() {
    [[ -n $forwarder ]] && kill "$forwarder" 2>/dev/null
    [[ -n $wrapper ]] && rm -f "$wrapper"
    return 0
}
trap cleanup EXIT

if [[ -n ${ULW_CALL_OUTSIDE_CONTAINER:-} ]]; then
    export ULW_CALL_PAGE_PORT=${ULW_CALL_PAGE_PORT:-48123}
    export no_proxy=${no_proxy:+$no_proxy,}198.18.0.0/24
    export NO_PROXY=$no_proxy
    pid=$(docker inspect -f '{{.State.Pid}}' "$ULW_CALL_OUTSIDE_CONTAINER")
    network=$(docker inspect -f '{{range $name, $_ := .NetworkSettings.Networks}}{{$name}}{{end}}' \
        "$ULW_CALL_OUTSIDE_CONTAINER")
    # This host, as the container sees it: its network's gateway.
    host=$(docker network inspect -f '{{(index .IPAM.Config 0).Gateway}}' "$network")
    nsenter --net="/proc/$pid/ns/net" -- python3 "$here/loopback_forward.py" \
        "$ULW_CALL_PAGE_PORT" "$host" &
    forwarder=$!
    wrapper=$(mktemp "${TMPDIR:-/tmp}/ulw-outside-chrome.XXXXXX")
    printf '#!/bin/sh\nexec nsenter --net=/proc/%s/ns/net -- %q "$@"\n' "$pid" "$ULW_E2E_CHROME" \
        >"$wrapper"
    chmod +x "$wrapper"
    export ULW_CALL_OUTSIDE_CHROME=$wrapper
fi

cd "$here"
# Exactly the lockfile: its sha512 integrity lines are the hash pins.
PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 npm ci --ignore-scripts --no-audit --no-fund >/dev/null
npx --no-install playwright test "$@"

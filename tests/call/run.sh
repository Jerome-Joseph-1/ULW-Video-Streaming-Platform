#!/usr/bin/env bash
# The call suite against a build tree, in one command:
#   tests/call/run.sh [build-dir, default build/ci] [playwright test arguments...]
# Needs Node 20 or later. The browser is the pinned Chrome for Testing that tests/e2e uses,
# fetched by fetch-chrome.sh.
#
# By default both peers run on this host against the LiveKit server of deploy/local/compose.yaml,
# which is started with its recorder if they are not running (and left running). The ingest spec
# also needs gst-launch-1.0 with the good and bad plugins, ffmpeg, cargo and GStreamer's
# development files (fetch-whipsink.sh builds whipsink), and the live_packager and ulw_sandbox
# targets of the same build. The direct-call and group-call specs need psql, a Postgres
# (ULW_TEST_DATABASE_URL) and the chat_server, ulw_migrate and ulw_devtoken targets instead of
# the harness; the recorder is started only when a spec that uses the harness runs. The
# environment can point the suite elsewhere:
#   LIVEKIT_API_URL, LIVEKIT_CLIENT_URL, LIVEKIT_API_KEY, LIVEKIT_API_SECRET
#       Another LiveKit, such as the one deploy/stunner/up.sh puts behind STUNner in the kind
#       sandbox. The local server is then left alone. The ingest spec's packager test needs a
#       recorder (egress) beside that server, which the sandbox does not run.
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

# The harness stands in for chat in call.spec.mjs and ingest.spec.mjs; the *-call specs run
# the product itself instead: chat_server, ulw_migrate and ulw_devtoken of the same build, on a
# scratch database of the Postgres at ULW_TEST_DATABASE_URL (default the local one of
# deploy/local/compose.yaml), created and dropped with psql.
export ULW_CALL_HARNESS=$build/tests/ulw_call_harness
export ULW_BUILD_DIR=$build
# Which specs the arguments select: those named, or all of them.
specs=()
for arg in "$@"; do
    [[ $arg == *.spec.mjs ]] && specs+=("$arg")
done
[[ ${#specs[@]} -eq 0 ]] && specs=(call.spec.mjs ingest.spec.mjs direct-call.spec.mjs group-call.spec.mjs)
needs_harness=
needs_chat=
for spec in "${specs[@]}"; do
    case $spec in
    *direct-call.spec.mjs | *group-call.spec.mjs) needs_chat=1 ;;
    *) needs_harness=1 ;;
    esac
done
if [[ -n $needs_harness && ! -x $ULW_CALL_HARNESS ]]; then
    echo "run.sh: $ULW_CALL_HARNESS missing; build the ulw_call_harness target" >&2
    exit 1
fi
if [[ -n $needs_chat ]]; then
    for target in apps/chat/chat_server apps/migrate/ulw_migrate tools/devtoken/ulw_devtoken; do
        if [[ ! -x $build/$target ]]; then
            echo "run.sh: $build/$target missing; build the ${target##*/} target" >&2
            exit 1
        fi
    done
fi

local_livekit=http://127.0.0.1:7880
# The fake key pair of deploy/local/compose.yaml, which the sandbox's LiveKit shares.
export LIVEKIT_API_KEY=${LIVEKIT_API_KEY:-ulw-dev-key}
export LIVEKIT_API_SECRET=${LIVEKIT_API_SECRET:-ulw-dev-secret-testtest123-not-a-real-secret}
export LIVEKIT_API_URL=${LIVEKIT_API_URL:-$local_livekit}
export LIVEKIT_CLIENT_URL=${LIVEKIT_CLIENT_URL:-ws://127.0.0.1:7880}
if [[ $LIVEKIT_API_URL == "$local_livekit" ]]; then
    # The recorder only for the specs that relay through it (the harness's).
    docker compose -f "$root/deploy/local/compose.yaml" --profile calls up -d --wait livekit \
        ${needs_harness:+egress} >/dev/null
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

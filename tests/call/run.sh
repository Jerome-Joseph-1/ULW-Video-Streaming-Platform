#!/usr/bin/env bash
# The call suite against a build tree, in one command:
#   tests/call/run.sh [build-dir, default build/ci] [playwright test arguments...]
# Starts the LiveKit server of deploy/local/compose.yaml if it is not running (and leaves it
# running), and needs Node 20 or later. The browser is the pinned Chrome for Testing that
# tests/e2e uses, fetched by fetch-chrome.sh.
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
# The fake key pair of deploy/local/compose.yaml.
export LIVEKIT_API_KEY=ulw-dev-key
export LIVEKIT_API_SECRET=ulw-dev-secret-testtest123-not-a-real-secret
export LIVEKIT_API_URL=http://127.0.0.1:7880
export LIVEKIT_CLIENT_URL=ws://127.0.0.1:7880

docker compose -f "$root/deploy/local/compose.yaml" --profile calls up -d --wait livekit >/dev/null
ULW_E2E_CHROME=${ULW_E2E_CHROME:-$("$here/fetch-chrome.sh")}
export ULW_E2E_CHROME

cd "$here"
# Exactly the lockfile: its sha512 integrity lines are the hash pins.
PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 npm ci --ignore-scripts --no-audit --no-fund >/dev/null
exec npx --no-install playwright test "$@"

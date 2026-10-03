#!/usr/bin/env bash
# The demo's smoke test: starts the demo stack (demo/up.sh, with its settings: DEMO_BUILD=1
# builds the images from this checkout), then drives every tab in headless Chrome for Testing
# with a fake camera and microphone (smoke.spec.mjs). Needs Node 20 or later, curl and unzip
# for the pinned Chrome, and Docker.
#
#   demo/smoke/run.sh [playwright test arguments...]
#   DEMO_SKIP_UP=1   use a demo stack that is already up
#   DEMO_DOWN=1      stop it afterwards (demo/down.sh --wipe)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

if [[ ${DEMO_SKIP_UP:-0} != 1 ]]; then
    "$here/../up.sh"
fi
export DEMO_URL=${DEMO_URL:-http://localhost:${DEMO_PORT:-8080}}
ULW_E2E_CHROME=${ULW_E2E_CHROME:-$("$root/tests/call/fetch-chrome.sh")}
export ULW_E2E_CHROME

cd "$here"
PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 npm ci --ignore-scripts --no-audit --no-fund >/dev/null
status=0
npx --no-install playwright test "$@" || status=$?
if ((status != 0)); then
    mkdir -p results
    docker compose -f "$here/../compose.yaml" ps -a > results/compose-ps.txt 2>&1 || true
    docker compose -f "$here/../compose.yaml" logs --no-color > results/compose.log 2>&1 || true
    echo "smoke: failed; the stack's logs are in demo/smoke/results/compose.log" >&2
fi
if [[ ${DEMO_DOWN:-0} == 1 ]]; then
    "$here/../down.sh" --wipe
fi
exit "$status"

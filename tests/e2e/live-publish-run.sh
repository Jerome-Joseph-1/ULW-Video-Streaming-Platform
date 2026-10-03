#!/usr/bin/env bash
# Live publishing through the gateway's stream service, end to end (docs/adr/0092), and with
# LiveKit's webhooks doing the starting and ending (docs/adr/0093; its LiveKit posts them to
# 127.0.0.1:7890, or ULW_E2E_WEBHOOK_PORT):
#   tests/e2e/live-publish-run.sh [build-dir, default build/ci] [playwright test arguments...]
# Needs the Postgres and MinIO of deploy/local/compose.yaml, its LiveKit and egress (the calls
# profile, started here if LIVEKIT_API_URL is not set and left running), ffmpeg and Node 20 or
# later, and the gateway_server, transcode_worker, live_packager, ulw_sandbox, ulw_migrate and
# ulw_devtoken targets of the build.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ULW_BUILD_DIR=$(realpath "${1:-$root/build/ci}")
export ULW_BUILD_DIR
shift $(($# > 0 ? 1 : 0))

if [[ -z ${LIVEKIT_API_URL:-} ]]; then
    docker compose -f "$root/deploy/local/compose.yaml" --profile calls up -d --wait livekit \
        egress >/dev/null
fi

ULW_E2E_CHROME=${ULW_E2E_CHROME:-$("$root/tests/call/fetch-chrome.sh")}
export ULW_E2E_CHROME

cd "$here"
PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 npm ci --ignore-scripts --no-audit --no-fund >/dev/null
exec npx --no-install playwright test -c playwright.publish.config.mjs "$@"

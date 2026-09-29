#!/usr/bin/env bash
# The live packager end to end, in one command:
#   tests/e2e/live-run.sh [build-dir, default build/ci] [playwright test arguments...]
# Needs the MinIO of deploy/local/compose.yaml, ffmpeg and Node 20 or later. It starts its own
# live_packager and test publisher, and plays the stream in hls.js from the bucket. Separate
# from run.sh, which starts the VOD stack.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ULW_BUILD_DIR=$(realpath "${1:-$root/build/ci}")
export ULW_BUILD_DIR
shift $(($# > 0 ? 1 : 0))

# The same browser, pinned the same way, as run.sh: Playwright's own Chromium has no H.264 or
# AAC decoder, which is all the packager copies through.
chrome_version=141.0.7390.37
chrome_sha256=b294238e5ac3e04f5f7fc5e5c017692bf64635e8824c3a7a44bea6a7ef6e147d
if [[ -z ${ULW_E2E_CHROME:-} ]]; then
    cache=${ULW_E2E_CACHE:-$here/.browsers}
    dir=$cache/chrome-headless-shell-$chrome_version
    shell=$dir/chrome-headless-shell-linux64/chrome-headless-shell
    if [[ ! -x $shell ]]; then
        mkdir -p "$cache"
        zip=$(mktemp "$cache/download.XXXXXX")
        if ! curl -fsSL -o "$zip" "https://storage.googleapis.com/chrome-for-testing-public/$chrome_version/linux64/chrome-headless-shell-linux64.zip" ||
            ! echo "$chrome_sha256  $zip" | sha256sum --check --quiet -; then
            rm -f "$zip"
            echo "live-run.sh: Chrome for Testing download failed or did not match its SHA-256" >&2
            exit 1
        fi
        rm -rf "$dir"
        mkdir -p "$dir"
        unzip -q "$zip" -d "$dir"
        rm -f "$zip"
    fi
    export ULW_E2E_CHROME=$shell
fi

cd "$here"
PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 npm ci --ignore-scripts --no-audit --no-fund >/dev/null
exec npx --no-install playwright test -c playwright.live.config.mjs "$@"

#!/usr/bin/env bash
# The end-to-end suite against a build tree, in one command:
#   tests/e2e/run.sh [build-dir, default build/ci] [playwright test arguments...]
# Needs the Postgres and MinIO of deploy/local/compose.yaml, ffmpeg, psql and Node 20 or later.
# It starts its own gateway_server and transcode_worker, on a scratch database it drops after.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ULW_BUILD_DIR=$(realpath "${1:-$root/build/ci}")
export ULW_BUILD_DIR
shift $(($# > 0 ? 1 : 0))

# Playwright's own Chromium is built without the H.264 and AAC decoders, and the worker writes
# nothing else, so hls.js would refuse every rendition. Chrome for Testing is the same browser
# with them: this is the build Playwright 1.56.1's Chromium was cut from, so the protocol
# matches. Pinned by SHA-256 like every other download (third_party/README.md).
chrome_version=141.0.7390.37
chrome_sha256=b294238e5ac3e04f5f7fc5e5c017692bf64635e8824c3a7a44bea6a7ef6e147d
if [[ -z ${ULW_E2E_CHROME:-} ]]; then
    cache=${ULW_E2E_CACHE:-$here/.browsers}
    dir=$cache/chrome-headless-shell-$chrome_version
    shell=$dir/chrome-headless-shell-linux64/chrome-headless-shell
    if [[ ! -x $shell ]]; then
        mkdir -p "$cache"
        zip=$(mktemp "$cache/download.XXXXXX")
        trap 'rm -f "$zip"' EXIT
        curl -fsSL -o "$zip" "https://storage.googleapis.com/chrome-for-testing-public/$chrome_version/linux64/chrome-headless-shell-linux64.zip"
        echo "$chrome_sha256  $zip" | sha256sum --check --quiet -
        rm -rf "$dir"
        mkdir -p "$dir"
        unzip -q "$zip" -d "$dir"
    fi
    export ULW_E2E_CHROME=$shell
fi

cd "$here"
# Exactly the lockfile: its sha512 integrity lines are the hash pins.
PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 npm ci --ignore-scripts --no-audit --no-fund >/dev/null
exec npx --no-install playwright test "$@"

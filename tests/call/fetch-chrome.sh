#!/usr/bin/env bash
# Prints the path of the pinned Chrome for Testing headless shell, downloading it first when the
# cache lacks it. The same build and SHA-256 as tests/e2e/run.sh, into the same cache, so the two
# suites share one download.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)

chrome_version=141.0.7390.37
chrome_sha256=b294238e5ac3e04f5f7fc5e5c017692bf64635e8824c3a7a44bea6a7ef6e147d
cache=${ULW_E2E_CACHE:-$here/../e2e/.browsers}
dir=$cache/chrome-headless-shell-$chrome_version
shell=$dir/chrome-headless-shell-linux64/chrome-headless-shell
if [[ ! -x $shell ]]; then
    mkdir -p "$cache"
    zip=$(mktemp "$cache/download.XXXXXX")
    trap 'rm -f "$zip"' EXIT
    if ! curl -fsSL -o "$zip" "https://storage.googleapis.com/chrome-for-testing-public/$chrome_version/linux64/chrome-headless-shell-linux64.zip" ||
        ! echo "$chrome_sha256  $zip" | sha256sum --check --quiet -; then
        echo "fetch-chrome.sh: Chrome for Testing download failed or did not match its SHA-256" >&2
        exit 1
    fi
    rm -rf "$dir"
    mkdir -p "$dir"
    unzip -q "$zip" -d "$dir"
fi
echo "$shell"

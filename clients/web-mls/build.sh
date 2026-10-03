#!/usr/bin/env bash
# Builds dist/: the client as WebAssembly with its JavaScript glue, in a Docker image whose every
# input is pinned (Dockerfile.build: the rust image by digest, the wasm32 standard library and
# wasm-bindgen by SHA-256), so the only thing the machine needs is Docker (ADR-0099). Crates are
# pinned by Cargo.lock, by version and SHA-256 (cargo build --locked).
# The output is the same bytes on every run; dist/SHA256SUMS records them, and `./build.sh
# --check` rebuilds into a scratch directory and fails if anything differs from dist/.
#
# Behind a TLS-intercepting proxy, set ULW_EXTRA_CA to its CA bundle; cargo uses it for the
# crate downloads, and it changes nothing in the output.
set -euo pipefail

IMAGE="ulw-web-mls-build:1"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
check=0
if [[ "${1:-}" == "--check" ]]; then
    check=1
fi

out="$here/dist"
if ((check)); then
    out="$(mktemp -d)"
    trap 'rm -rf "$out"' EXIT
fi
mkdir -p "$out"

ca_args=()
if [[ -n "${ULW_EXTRA_CA:-}" ]]; then
    ca_args=(-v "$ULW_EXTRA_CA:/extra-ca.crt:ro" -e CARGO_HTTP_CAINFO=/extra-ca.crt)
fi

# linux/amd64 everywhere, Apple silicon included (under emulation), so the one pinned
# wasm-bindgen binary serves and every machine builds alike.
docker build --quiet --platform linux/amd64 -t "$IMAGE" -f "$here/Dockerfile.build" "$here" >/dev/null
docker run --rm --platform linux/amd64 \
    -v "$here:/src:ro" -v "$out:/out" "${ca_args[@]}" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -e CARGO_TARGET_DIR=/build/target -e SOURCE_DATE_EPOCH=0 \
    "$IMAGE" bash -euo pipefail -c '
        cd /src
        cargo build --locked --release --target wasm32-unknown-unknown --lib
        rm -f /out/web_mls* /out/mls-room.js /out/SHA256SUMS
        wasm-bindgen --target web --out-dir /out \
            /build/target/wasm32-unknown-unknown/release/web_mls.wasm
        cp /src/js/mls-room.js /out/
        cd /out
        sha256sum mls-room.js web_mls.d.ts web_mls.js web_mls_bg.wasm web_mls_bg.wasm.d.ts \
            > SHA256SUMS
        chown -R "$HOST_UID:$HOST_GID" /out
    '

if ((check)); then
    if diff -r "$out" "$here/dist"; then
        echo "dist/ matches a fresh build"
    else
        echo "dist/ differs from a fresh build" >&2
        exit 1
    fi
else
    cat "$out/SHA256SUMS"
fi

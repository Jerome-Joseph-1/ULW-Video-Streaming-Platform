#!/usr/bin/env bash
# Builds dist/: the client as WebAssembly with its JavaScript glue, in a Docker image pinned by
# digest, so the only thing the machine needs is Docker (ADR-0099). Everything is pinned:
#   - rustc 1.94.1 (the FFI bridge's, ADR-0044): the official rust image, by digest;
#   - the wasm32 target: rustup installs it for that toolchain, checked against its signed
#     manifest;
#   - every crate: Cargo.lock, by version and SHA-256 (cargo build --locked);
#   - wasm-bindgen-cli 0.2.129, the version Cargo.lock pins for the library half: the release
#     tarball, checked against its SHA-256 below.
# The output is the same bytes on every run; dist/SHA256SUMS records them, and `./build.sh
# --check` rebuilds into a scratch directory and fails if anything differs from dist/.
#
# Behind a TLS-intercepting proxy, set ULW_EXTRA_CA to its CA bundle; it is mounted for the
# downloads only and changes nothing in the output.
set -euo pipefail

IMAGE="rust:1.94.1-slim-bookworm@sha256:cf9dd0ec73e75f827fe59123fff9dc65af1a1c8363c3c31ee8d7f8ad0b6a5fb2"
BINDGEN_VERSION="0.2.129"
BINDGEN_URL="https://github.com/wasm-bindgen/wasm-bindgen/releases/download/${BINDGEN_VERSION}/wasm-bindgen-${BINDGEN_VERSION}-x86_64-unknown-linux-musl.tar.gz"
BINDGEN_SHA256="82d12bb940e2d4e72e0d5605387fc1b8ca179044e012b620f0ce4e7440e8320e"

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
    ca_args=(-v "$ULW_EXTRA_CA:/extra-ca.crt:ro" -e SSL_CERT_FILE=/extra-ca.crt
        -e CARGO_HTTP_CAINFO=/extra-ca.crt -e CURL_CA_BUNDLE=/extra-ca.crt)
fi

# linux/amd64 everywhere, Apple silicon included (under emulation), so the one pinned
# wasm-bindgen binary serves and every machine builds alike.
docker run --rm --platform linux/amd64 \
    -v "$here:/src:ro" -v "$out:/out" "${ca_args[@]}" \
    -e BINDGEN_URL="$BINDGEN_URL" -e BINDGEN_SHA256="$BINDGEN_SHA256" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -e CARGO_TARGET_DIR=/build/target -e SOURCE_DATE_EPOCH=0 \
    "$IMAGE" bash -euo pipefail -c '
        apt-get update -qq >/dev/null
        apt-get install -y -qq --no-install-recommends curl ca-certificates >/dev/null
        mkdir -p /build/bin
        curl -fsSL "$BINDGEN_URL" -o /build/bindgen.tgz
        echo "$BINDGEN_SHA256  /build/bindgen.tgz" | sha256sum -c --quiet
        tar -xzf /build/bindgen.tgz -C /build/bin --strip-components=1
        cd /src
        # rust-toolchain.toml names 1.94.1 and the wasm32 target; rustup adds the target.
        rustup show active-toolchain >/dev/null
        cargo build --locked --release --target wasm32-unknown-unknown --lib
        rm -f /out/web_mls* /out/mls-room.js /out/SHA256SUMS
        /build/bin/wasm-bindgen --target web --out-dir /out \
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

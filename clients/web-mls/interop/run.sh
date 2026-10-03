#!/usr/bin/env bash
# The interop check (ADR-0099): the WebAssembly client in dist/ and the FFI bridge's own code
# share MLS groups through a real chat_server on a scratch Postgres database. It builds the
# bridge (cargo, offline, from its Cargo.lock, as CMake does) and ffi_peer, then runs
# interop.mjs under Node 22 or later. With --browser it also runs browser.mjs: example.html in
# two Chromium contexts through Playwright (which must be installed, with its Chromium).
#
#   ULW_TEST_DATABASE_URL  an admin URL of a Postgres to make the scratch database in
#                          (default: the local ulw-pg, postgresql://postgres:testtest123@127.0.0.1:55432/postgres)
#   ULW_CHAT_BIN           a chat_server binary to run; without it the chat image is run with
#                          host networking
#   ULW_CHAT_IMAGE         that image (default ghcr.io/jerome-joseph-1/ulw-chat:main)
#   ULW_CHAT_PORT          the chat port (default 9171; the node port is the next one)
#   ULW_MLS_TARGET_DIR     cargo's target directory for the bridge (default: a temporary one,
#                          removed afterwards)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bridge="$here/../../../infra/e2ee/mls_ffi_bridge"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
target="${ULW_MLS_TARGET_DIR:-$work/target}"

(cd "$bridge" && RUSTUP_AUTO_INSTALL=0 CARGO_TARGET_DIR="$target" \
    cargo build --release --locked --offline --quiet)
cc -std=c11 -D_GNU_SOURCE -O1 -Wall -Wextra -Werror -I "$bridge/include" \
    "$here/ffi_peer.c" "$target/release/libmls_ffi_bridge.a" -lpthread -ldl -lm \
    -o "$work/ffi_peer"

export ULW_FFI_PEER="$work/ffi_peer"
node "$here/interop.mjs"
if [[ "${1:-}" == "--browser" ]]; then
    node "$here/browser.mjs"
fi

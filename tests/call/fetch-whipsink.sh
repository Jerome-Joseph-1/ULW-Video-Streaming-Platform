#!/usr/bin/env bash
# Prints the directory holding GStreamer's whipsink, for GST_PLUGIN_PATH, building it first
# when the cache lacks it. whipsink is gst-plugins-rs's webrtchttp plugin, which Ubuntu does not
# package: the crate is fetched by version and SHA-256 and built against the system's GStreamer
# with whipsink/Cargo.lock, whose checksums pin every crate it pulls in, by the Rust release
# whipsink/rust-toolchain.toml names. Needs rustup and the GStreamer development packages
# (libgstreamer-plugins-bad1.0-dev brings the WebRTC library).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)

version=0.13.5
# crates.io's checksum of gst-plugin-webrtchttp-0.13.5.crate.
sha256=8ab7a2aabd1be8358cd627f1b2984b40824c76a1bb9f9957afb905aa30ff45c8
cache=${ULW_WHIPSINK_CACHE:-$here/.whipsink}
dir=$cache/gst-plugin-webrtchttp-$version
lib=$dir/target/release/libgstwebrtchttp.so

if ! pkg-config --exists gstreamer-webrtc-1.0 gstreamer-sdp-1.0; then
    echo "fetch-whipsink.sh: GStreamer's WebRTC development files are missing" \
        "(libgstreamer-plugins-bad1.0-dev)" >&2
    exit 1
fi
# The build is kept while its inputs are: the crates (the lock file), the compiler
# (whipsink/rust-toolchain.toml) and the GStreamer it links against.
inputs=$(sha256sum "$here/whipsink/Cargo.lock" "$here/whipsink/rust-toolchain.toml" | cut -d' ' -f1)
stamp="$inputs gstreamer-$(pkg-config --modversion gstreamer-webrtc-1.0)"
if [[ ! -f $lib ]] || [[ $(cat "$dir/.ulw-stamp" 2>/dev/null) != "$stamp" ]]; then
    mkdir -p "$cache"
    crate=$(mktemp "$cache/download.XXXXXX")
    trap 'rm -f "$crate"' EXIT
    if ! curl -fsSL --proto =https --proto-redir =https -o "$crate" \
        "https://static.crates.io/crates/gst-plugin-webrtchttp/gst-plugin-webrtchttp-$version.crate" ||
        ! echo "$sha256  $crate" | sha256sum --check --quiet -; then
        echo "fetch-whipsink.sh: the crate download failed or did not match its SHA-256" >&2
        exit 1
    fi
    rm -rf "$dir"
    tar -xzf "$crate" -C "$cache"
    cp "$here/whipsink/Cargo.lock" "$here/whipsink/rust-toolchain.toml" "$dir/"
    # rustup installs the pinned release, checked against the channel's signed manifest.
    (cd "$dir" && rustup toolchain install --no-self-update >/dev/null &&
        cargo build --release --locked --quiet) >&2
    echo "$stamp" >"$dir/.ulw-stamp"
fi
dirname "$lib"

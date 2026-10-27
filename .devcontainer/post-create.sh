#!/usr/bin/env bash
# Runs once, when the development container is created (devcontainer.json): what the build
# needs beyond the image, so the first `tools/dev/build.sh` works offline from here.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

# The OpenMLS bridge builds with cargo --locked --offline (ADR-0044), so its crates must be in
# the registry cache before CMake runs; rustup installs the release rust-toolchain.toml names.
if [[ -f infra/e2ee/mls_ffi_bridge/Cargo.lock ]]; then
    (cd infra/e2ee/mls_ffi_bridge && rustup toolchain install --no-self-update && cargo fetch --locked)
fi

# CMakeLists.txt puts ccache in front of the compiler when it finds it. The cache is on the
# codespace's disk, which a stop keeps: a rebuild after one compiles only what changed.
ccache --set-config=max_size=5G

# Where tools/dev keeps its state: the dev signing key, logs and pids. Git ignores it.
mkdir -p .dev

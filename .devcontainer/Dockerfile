# The development container (docs/development/cloud-dev.md, docs/adr/0140): the toolchain CI
# installs (.github/actions/setup/action.yml), plus what the live suites need (psql, GStreamer,
# coturn), on the Ubuntu release CI and the images build on (docs/adr/0030). Docker, Node, Go and
# Rust come from the features devcontainer.json names.
#
# Keep the first package list in step with the setup action's: a package added there for the
# build or the unit tests belongs here too.

# mcr.microsoft.com/devcontainers/base:ubuntu-24.04, 2026-10: Ubuntu 24.04 with a non-root
# `vscode` user that has sudo, git and the usual shell tools.
FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04@sha256:d94c97dd9cacf183d0a6fd12a8e87b526e9e928307674ae9c94139139c0c6eae

RUN apt-get update -q \
 && DEBIAN_FRONTEND=noninteractive apt-get install -qy --no-install-recommends \
        g++-14 clang-19 clang-tidy-19 clang-format-18 lld-19 libclang-rt-19-dev \
        ninja-build ccache mold jq pkg-config cmake \
        libssl-dev libcurl4-openssl-dev libpq-dev liburing-dev libjemalloc-dev zlib1g-dev \
        ffmpeg python3-yaml \
        postgresql-client coturn unzip \
        gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad gstreamer1.0-nice libgstreamer1.0-dev \
        libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

# CMake's presets name no compiler except `coverage` and `fuzz`; these make gcc-14 the default
# for the others, as CI's CC and CXX do.
ENV CC=gcc-14 CXX=g++-14

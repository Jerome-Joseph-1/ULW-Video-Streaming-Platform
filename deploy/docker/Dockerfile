# Images for the VOD plane, built from the repository root:
#
#   docker build -f deploy/docker/Dockerfile --target gateway --build-arg ULW_GIT_SHA=$(git rev-parse --short=12 HEAD) -t video-gateway .
#   docker build -f deploy/docker/Dockerfile --target worker  --build-arg ULW_GIT_SHA=... -t video-worker .
#
# gateway carries gateway_server, ulw_migrate and ulw_reaper (the video-gateway pod's init
# container runs the gateway image with ulw_migrate as its command, so schema and server always
# come from one build; docs/adr/0031; the upload reaper's CronJob runs it with ulw_reaper as its
# command, docs/adr/0049). worker carries
# transcode_worker, ulw_sandbox beside it, and ffmpeg.
#
# gateway is Ubuntu and worker is Debian. The worker's ffmpeg comes from Debian 13's security
# archive, which Ubuntu's ffmpeg (universe, fixed only in the paid ESM archive) is not
# (docs/adr/0074); the gateway has no ffmpeg and stays on the Ubuntu release CI and the dev
# machines build on (docs/adr/0030). Both images' binaries are built here, on Ubuntu: Debian's
# glibc start files carry no CET marking, so nothing linked on trixie is marked for IBT and
# SHSTK (tools/check-hardening.sh). The worker's binaries run on trixie's newer glibc and
# libstdc++, OpenSSL 3's libcrypto (EVP digests), libcurl 4 and libpq 5 through those
# libraries' stable sonames and symbol versions, so the Ubuntu release built on must never have
# a newer glibc or libstdc++ than trixie; the trixie workflow loads the image's binaries and
# runs the ffmpeg suites there to hold that.

# ubuntu:24.04 (noble), 2026-09.
ARG UBUNTU=ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3
# debian:trixie-slim (13.7), 2026-09-18.
ARG DEBIAN=debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a

FROM ${UBUNTU} AS apt
ENV UBUNTU_SNAPSHOT=20260930T000000Z

FROM apt AS build
RUN --mount=type=bind,source=deploy/docker/apt-install.sh,target=/usr/local/sbin/apt-install \
    --mount=type=secret,id=ca-bundle,mode=0444 \
    apt-install \
        ca-certificates=20260601~24.04.1 \
        openssl=3.0.13-0ubuntu3.16 \
        libssl3t64=3.0.13-0ubuntu3.16 \
        g++-14=14.2.0-4ubuntu2~24.04.1 \
        cmake=3.28.3-1build7 \
        ninja-build=1.11.1-2 \
        pkgconf=1.8.1-2build1 \
        libssl-dev=3.0.13-0ubuntu3.16 \
        libcurl4-openssl-dev=8.5.0-2ubuntu10.15 \
        libpq-dev=16.15-0ubuntu0.24.04.1 \
        liburing-dev=2.5-1build1
WORKDIR /src
COPY . .
ARG ULW_GIT_SHA=unknown
# No binary built here links jemalloc: only chat_server does (ADR-0081), and it has no image
# yet. Its image will need libjemalloc-dev=5.3.0-2build1 here and libjemalloc2=5.3.0-2build1 in
# its runtime, the versions its soaks measured.
RUN CC=gcc-14 CXX=g++-14 cmake --preset release -DULW_BUILD_TESTS=OFF -DULW_JEMALLOC=OFF \
        -DULW_GIT_SHA=${ULW_GIT_SHA} \
 && cmake --build --preset release \
        --target gateway_server ulw_migrate ulw_reaper transcode_worker ulw_sandbox \
 && install -D -m 0755 -t /out/gateway \
        build/release/apps/gateway/gateway_server build/release/apps/migrate/ulw_migrate \
        build/release/apps/reaper/ulw_reaper \
 && install -D -m 0755 -t /out/worker \
        build/release/apps/worker/transcode_worker build/release/apps/worker/ulw_sandbox

FROM apt AS runtime
RUN --mount=type=bind,source=deploy/docker/apt-install.sh,target=/usr/local/sbin/apt-install \
    --mount=type=secret,id=ca-bundle,mode=0444 \
    apt-install \
        ca-certificates=20260601~24.04.1 \
        openssl=3.0.13-0ubuntu3.16 \
        libcurl4t64=8.5.0-2ubuntu10.15 \
        libpq5=16.15-0ubuntu0.24.04.1 \
        libssl3t64=3.0.13-0ubuntu3.16 \
        liburing2=2.5-1build1 \
 && useradd --system --uid 10001 --user-group --no-create-home --shell /usr/sbin/nologin ulw

FROM runtime AS gateway
COPY --from=build /out/gateway/ /usr/local/bin/
USER 10001:10001
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/gateway_server"]

# The worker: Debian 13 (trixie), every package from one snapshot.debian.org timestamp
# (apt-install-debian.sh) that holds the ffmpeg security release pinned below (DSA-6361-1, in
# debian-security since 20260622T192543Z). A new DSA for ffmpeg, or for anything else here, is
# a bump of DEBIAN_SNAPSHOT and of the versions it names (RUNBOOK.md, "Updating the worker's
# ffmpeg").
FROM ${DEBIAN} AS worker
ENV DEBIAN_SNAPSHOT=20260930T100000Z
RUN --mount=type=bind,source=deploy/docker/apt-install-debian.sh,target=/usr/local/sbin/apt-install-debian \
    --mount=type=secret,id=ca-bundle,mode=0444 \
    apt-install-debian \
        ca-certificates=20250419 \
        openssl=3.5.7-1~deb13u3 \
        libcurl4t64=8.14.1-2+deb13u5 \
        libpq5=17.11-0+deb13u1 \
        libssl3t64=3.5.7-1~deb13u3 \
        openssl-provider-legacy=3.5.7-1~deb13u3 \
        ffmpeg=7:7.1.5-0+deb13u1 \
 && useradd --system --uid 10001 --user-group --no-create-home --shell /usr/sbin/nologin ulw \
 && install -d -o 10001 -g 10001 -m 0700 /var/cache/ulw-worker
COPY --from=build /out/worker/ /usr/local/bin/
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/transcode_worker"]

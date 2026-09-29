# Images for the VOD plane, built from the repository root:
#
#   docker build -f deploy/docker/Dockerfile --target gateway --build-arg ULW_GIT_SHA=$(git rev-parse --short=12 HEAD) -t video-gateway .
#   docker build -f deploy/docker/Dockerfile --target worker  --build-arg ULW_GIT_SHA=... -t video-worker .
#
# gateway carries gateway_server, ulw_migrate and ulw_reaper (the video-gateway pod's init
# container runs the gateway image with ulw_migrate as its command, so schema and server always
# come from one build; docs/adr/0031; the upload reaper's CronJob runs it with ulw_reaper as its
# command, docs/adr/0039). worker carries
# transcode_worker, ulw_sandbox beside it, and ffmpeg.
#
# Builder and runtime are the same Ubuntu release, which is also what CI and the dev machines
# build on, so the shared libraries the binaries link are the ones they were compiled against
# (docs/adr/0030).

# ubuntu:24.04 (noble), 2026-09.
ARG UBUNTU=ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3

FROM ${UBUNTU} AS apt
ENV UBUNTU_SNAPSHOT=20260925T000000Z

FROM apt AS build
RUN --mount=type=bind,source=deploy/docker/apt-install.sh,target=/usr/local/sbin/apt-install \
    --mount=type=secret,id=ca-bundle,mode=0444 \
    apt-install \
        ca-certificates=20260601~24.04.1 \
        openssl=3.0.13-0ubuntu3.15 \
        g++-14=14.2.0-4ubuntu2~24.04.1 \
        cmake=3.28.3-1build7 \
        ninja-build=1.11.1-2 \
        pkgconf=1.8.1-2build1 \
        libssl-dev=3.0.13-0ubuntu3.15 \
        libcurl4-openssl-dev=8.5.0-2ubuntu10.15 \
        libpq-dev=16.15-0ubuntu0.24.04.1 \
        liburing-dev=2.5-1build1
WORKDIR /src
COPY . .
ARG ULW_GIT_SHA=unknown
RUN CC=gcc-14 CXX=g++-14 cmake --preset release -DULW_BUILD_TESTS=OFF \
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
        openssl=3.0.13-0ubuntu3.15 \
        libcurl4t64=8.5.0-2ubuntu10.15 \
        libpq5=16.15-0ubuntu0.24.04.1 \
        libssl3t64=3.0.13-0ubuntu3.15 \
        liburing2=2.5-1build1 \
 && useradd --system --uid 10001 --user-group --no-create-home --shell /usr/sbin/nologin ulw

FROM runtime AS gateway
COPY --from=build /out/gateway/ /usr/local/bin/
USER 10001:10001
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/gateway_server"]

FROM runtime AS worker
RUN --mount=type=bind,source=deploy/docker/apt-install.sh,target=/usr/local/sbin/apt-install \
    --mount=type=secret,id=ca-bundle,mode=0444 \
    apt-install ffmpeg=7:6.1.1-3ubuntu5 \
 && install -d -o 10001 -g 10001 -m 0700 /var/tmp/ulw-worker
COPY --from=build /out/worker/ /usr/local/bin/
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/transcode_worker"]

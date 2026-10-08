# The demo's gateway: gateway_server and ulw_migrate from the gateway image, run in the live
# packager's image, so that the stream service's `process` packager runtime (ULW_LIVE_PACKAGER=
# process, one live_packager child per stream) finds live_packager, ulw_sandbox and ffmpeg beside
# it. Nothing is compiled here; up.sh builds it from whichever images it was given (the
# published :main ones, or DEMO_BUILD=1's local ones).
ARG GATEWAY_IMAGE=ghcr.io/jerome-joseph-1/ulw-video-gateway:main
ARG PACKAGER_IMAGE=ghcr.io/jerome-joseph-1/ulw-live-packager:main
FROM ${GATEWAY_IMAGE} AS gateway

FROM ${PACKAGER_IMAGE}
USER root
# gateway_server links liburing, which the packager's Debian image does not carry; its soname
# is stable, so Ubuntu's copy is loaded as is (the image's binaries are all built on Ubuntu).
RUN --mount=from=gateway,source=/,target=/gateway \
    lib=$(dirname "$(ls /usr/lib/*/libpq.so.5)") \
 && cp -a /gateway/usr/lib/*/liburing.so.2* "$lib"/ \
 && cp -a /gateway/usr/local/bin/gateway_server /gateway/usr/local/bin/ulw_migrate /usr/local/bin/ \
 && ! ldd /usr/local/bin/gateway_server | grep 'not found'
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/gateway_server"]

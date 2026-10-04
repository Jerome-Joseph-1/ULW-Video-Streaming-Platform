# The demo's page and its one origin (demo/release/compose.yaml): nginx with the page (demo/web),
# the OpenMLS browser client (clients/web-mls/dist, served at /mls/ as application/wasm) and the
# proxy config baked in, so the release compose mounts nothing from a checkout. Context: the
# repository root.
# nginx:1.29-alpine, 2026-10, the digest demo/compose.yaml pins.
FROM docker.io/library/nginx@sha256:5616878291a2eed594aee8db4dade5878cf7edcb475e59193904b198d9b830de
COPY demo/nginx/default.conf /etc/nginx/conf.d/default.conf
COPY demo/web/ /usr/share/nginx/html/
# The path demo/nginx/default.conf's /mls/ location aliases (the checkout's mount in demo/compose.yaml).
COPY clients/web-mls/dist/ /repo/clients/web-mls/dist/
RUN nginx -t

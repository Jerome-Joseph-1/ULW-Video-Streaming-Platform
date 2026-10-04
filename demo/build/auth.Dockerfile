# The demo's token issuer and one-time setup (demo/auth/server.mjs), for demo/release/compose.yaml.
# Node's standard library only. Context: the repository root.
# node:22-alpine, 2026-10, the digest demo/compose.yaml pins.
FROM docker.io/library/node@sha256:0a7108bf6c7bf5de370ffb1a3ed6be93d405b43ff159f681a8d18c0e2bc2e402
COPY demo/auth/server.mjs /demo/server.mjs
CMD ["node", "/demo/server.mjs"]

# ULW

A C++23 video platform for Linux: resumable uploads streamed straight to object storage, an
FFmpeg HLS transcode worker, and a realtime plane for chat, calls and live streams.

## Build and test

Needs GCC 14 or Clang 19 (Clang 18 lacks `std::expected` in libstdc++; ADR-0022), CMake 3.28+,
Ninja, and development packages for OpenSSL 3, libcurl, libpq and liburing 2.5+.

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

Presets: `dev`, `debug`, `asan`, `tsan`, `ci` (warnings are errors), `release` (LTO), `fuzz`
(clang). Test presets: `unit`, `tsan-unit`, `conformance`, `integration`. Checks:

```sh
tools/check-boundaries.sh   # layering rules
tools/check-format.sh       # clang-format; --fix to apply
tools/run-clang-tidy.sh build/dev [files...]
python3 tools/check-docs.py # ADR numbering and sections, route/doc coverage
tools/e2ee_diagnostic_check.sh [<base> <head>]  # chat service and router unchanged, phase-2..phase-3
```

## Run locally

```sh
docker compose -f deploy/local/compose.yaml up -d --wait   # Postgres 16 and MinIO on loopback
ULW_DATABASE_URL=postgresql://postgres:testtest123@127.0.0.1:55432/postgres \
    build/dev/apps/migrate/ulw_migrate
```

Then start `gateway_server` (`apps/gateway`); settings come from a TOML file, `ULW_*`
environment variables and flags (ADR-0040). For the whole platform behind Envoy in a kind
cluster (needs Docker): `make e2e-up`, `make e2e-test`, `make e2e-down`; also `make e2e-load`,
`make e2e-stunner` and `make validate-manifests`. Shipping to the real cluster is in
[`deploy/askedin/RUNBOOK.md`](deploy/askedin/RUNBOOK.md).

## Architecture

```
client -> Envoy -> gateway_server ---- Postgres (catalog, job queue, chat rooms)
                     |  presigned URLs   S3 / R2 (raw uploads, HLS renditions)
                     v
        transcode_worker <- job queue     ulw_reaper (CronJob: abandoned uploads)
                     |
                     +-> ffmpeg (sandboxed subprocess)

client -> Envoy -> chat_server x N  (one owner node per room, nodes forward to it)
client -> STUNner -> LiveKit (SFU: ICE, DTLS, SRTP)      ingest -> live_packager -> R2 (HLS)
```

Binaries live in `apps/`: `gateway_server` (uploads, catalog, playback), `transcode_worker`,
`chat_server`, `live_packager`, `ulw_reaper`, `ulw_migrate` (schema init container).
Playback returns rewritten playlists with presigned URLs; segment bytes never pass through
the gateway (ADR-0002). Identity is Askedin's JWT, verified against its JWKS (ADR-0018).

Layering, ports and adapters, enforced by `tools/check-boundaries.sh`:

- `core/` domain model and `core/ports/` interfaces; no OS or vendor includes
- `infra/*` adapters, one vendor each (`storage/s3`, `postgres`, `ffmpeg`, `srt`, `e2ee`, ...)
- `net/` reactor (io_uring, epoll fallback) and transports (plain, TLS); `os/` RAII handles
- `http/` HTTP/1.1 and upload contract; `codec/` sans-IO ws, sdp and rtp codecs
- `rt/` room registry, router and node channel; `ops/` config, logs, metrics, health
- `apps/` one directory per binary; `main.cpp` is the only place concrete adapters are named

## Documentation

- [`docs/adr/`](docs/adr/README.md) architecture decisions, one per file, immutable
- [`docs/integration/`](docs/integration/README.md) the contract for Askedin's app and backend teams
- [`docs/operations/`](docs/operations/soak.md) soak procedure and results
- [`docs/operations/testing.md`](docs/operations/testing.md) mutation testing of the unit suites

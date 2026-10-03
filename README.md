# ULW

A C++23 video platform for Linux: resumable uploads streamed straight to object storage, an
FFmpeg HLS transcode worker, and a realtime plane for chat, calls and live streams.

## Build and test

Needs GCC 14 or Clang 19 (Clang 18 lacks `std::expected` in libstdc++; ADR-0022), CMake 3.28+,
Ninja, jq (`tools/run-clang-tidy.sh`), and development packages for OpenSSL 3, libcurl, libpq
and liburing 2.5+. The OpenMLS bridge (`ULW_BUILD_MLS`) needs Rust and cargo, at the toolchain
pinned in `infra/e2ee/mls_ffi_bridge/rust-toolchain.toml`; `-DULW_BUILD_MLS=OFF` skips it.
jemalloc is linked into `chat_server` only (ADR-0081) and skipped under sanitizers;
`-DULW_JEMALLOC=OFF` skips it.

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

Presets: `dev`, `debug`, `asan`, `tsan`, `ci` (warnings are errors), `coverage` (clang-19,
source-based coverage), `release` (LTO), `fuzz` (clang). Test presets: `dev`, `ci`, `unit` (the
`asan` build), `tsan-unit`, `conformance`, `integration`. Checks:

```sh
tools/check-boundaries.sh   # layering rules
tools/check-format.sh       # clang-format; --fix to apply
tools/run-clang-tidy.sh build/dev [files...]
python3 tools/check-docs.py # ADR numbering and sections, route/doc coverage
tools/e2ee_diagnostic_check.sh [<base> <head>]  # chat service and router unchanged, phase-2..phase-3
                                                # (without arguments, a no-op until both tags are pushed)
```

Security checks (ADR-0072; the scanners are fetched, pinned, into `tools/security/.tools`):

```sh
tools/check-hardening.sh build/ci        # PIE, full RELRO, NX, stack protector, CET, FORTIFY
tools/security/osv-scan.sh               # lock files and vendored C/C++ against OSV
tools/security/trivy-config.sh           # deploy/ manifests, kustomizations, Dockerfiles
tools/security/trivy-image.sh IMAGE...   # HIGH/CRITICAL fixable CVEs in a built image
tools/security/lint-workflows.sh         # actionlint and zizmor over .github/
```

SonarQube Cloud analyses the repository in CI (the `coverage` job of `.github/workflows/ci.yml`,
ADR-0079, ADR-0083) on the Free plan, with the `coverage` preset's compile database and measured
coverage: `llvm-cov` over the unit and integration labels, run in four shards
(`COVERAGE_SHARD=i/4 tools/coverage.sh build/coverage unit integration`) and merged
(`COVERAGE_PROFILES=<dir> COVERAGE_ENFORCE=0 COVERAGE_SONAR=1 tools/coverage.sh
build/coverage`), coverage.py over the Python unit tests
(`tools/coverage-python.sh build/coverage-python.xml`) and the Go modules' tests
(`tools/coverage-go.sh build/coverage-go.out`); settings are in `sonar-project.properties`.
It is advisory (`sonar.qualitygate.wait=false`): the quality gate, whose new-code condition is
80% coverage, is reported on the dashboard and the pull request and never fails the job. It
needs the `SONAR_TOKEN` secret and Automatic Analysis turned off in the
project's settings. Pull requests from forks get no secrets, so no analysis. If the repository ever becomes private, delete `SONAR_TOKEN` to stay free:
the job then skips itself.

## Run locally

```sh
docker compose -f deploy/local/compose.yaml up -d --wait   # Postgres 16 and MinIO on loopback
docker compose -f deploy/local/compose.yaml --profile calls up -d --wait   # also LiveKit, Redis, egress
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
client -> Envoy -> gateway_server ---- Postgres (catalog, job queue)
                     |  presigned URLs   S3 / R2 (raw uploads, HLS renditions)
                     v
        transcode_worker <- job queue     ulw_reaper (CronJob: abandoned uploads)
                     |
                     +-> ffmpeg (sandboxed subprocess)

client -> Envoy -> chat_server x N ---- Postgres (chat rooms, messages)
                    (one owner node per room, nodes forward to it)
client -> STUNner -> LiveKit (SFU: ICE, DTLS, SRTP)
publisher -WHIP-> LiveKit -egress (SRT)-> live_packager -> R2 (HLS)   (ADR-0053)
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

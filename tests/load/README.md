# Load tools

Three standalone Python 3 scripts (standard library only, no installs needed) for exercising the
gateway's resumable upload API (`apps/gateway/src/routes.hpp`) under load. They join the existing
C++ load tools in this directory (`gateway_load.cpp`, `loadgen.cpp`).

**These are for the local sandbox and local runs only** — against a `gateway_server` you started
yourself on `127.0.0.1`. Never point them at a real deployment: `slowloris.py` and `flood.py` are
deliberately abusive traffic shapes, and running them against real infrastructure or anyone else's
service would be a denial-of-service attack, not a test.

## upload_load.py

Drives many concurrent resumable uploads and reports throughput, latency and error counts.

```
tests/load/upload_load.py --url http://127.0.0.1:8080 \
    --devtoken-key /tmp/dev-key.json --issuer local-load --users 20 \
    --uploads 20 --size 16777216 \
    --scrape-metrics http://127.0.0.1:8080
```

Tokens either come from `--token-file` (one bearer token per line, round-robined across
concurrent uploads because the gateway admits at most 3 uploads per user) or are minted on the
fly with `--devtoken-key <key> --issuer <iss> --users <n>`, which runs the repo's `ulw_devtoken`
binary (`--devtoken <path-to-it>`, default `build/ci/tools/devtoken/ulw_devtoken`).

Each upload holds one keep-alive connection from create to commit, so N concurrent uploads are N
open sockets. `--rate <bytes/s>` streams each PATCH body at that rate over it (16384, twice the
gateway's 8 KiB/s minimum body rate, is the slowest that stays clear of a 408), so the socket stays
open as long as a slow client's would. `--payload <file>` uploads that file's bytes, for uploads
the worker can transcode.

`--scrape-metrics <url>` samples `/metrics` every 5 s during the run and folds `uploads_in_flight`
and `connections_current` (the gauges `apps/gateway/src/gateway.cpp` exposes that describe load
in flight) into the JSON report.

## slowloris.py

Opens many sockets and trickles bytes into them just under the gateway's documented timeouts
(`apps/gateway/src/gateway.hpp`'s `Limits`, enforced in `connection.cpp`): a 10 s header timeout,
a 30 s body idle timeout, and a floor of 8 KiB/s averaged over each 30 s window a body is read.

```
tests/load/slowloris.py --url http://127.0.0.1:8080 --connections 50 --duration 60 \
    --legit-url http://127.0.0.1:8080
```

`--mode headers` (the default) trickles an unfinished header block one byte at a time.
`--mode body` completes a small PATCH's headers, then trickles the body below the 8 KiB/s floor.

`--legit-url` runs a concurrent health-check loop (`GET /api/v1/healthz`, optionally with
`--legit-token`) and reports its p50/p99 latency and error rate, to show ordinary traffic isn't
degraded while the slow connections are open.

## flood.py

Opens many short-lived connections as fast as it can: unauthenticated requests, a deliberately
oversized header the parser should reject outright, and — with `--token` — small valid requests,
all mixed together per worker.

```
tests/load/flood.py --url http://127.0.0.1:8080 --duration 30 --rate 500 \
    --legit-url http://127.0.0.1:8080 --token "$TOKEN"
```

`--rate` bounds combined requests/s across all workers (0 is unbounded, limited only by
`--workers`). `--legit-url`/`--legit-token` work the same way as in `slowloris.py`.

## A minimal local run

Build the gateway and its helpers, start a scratch Postgres database and MinIO bucket, mint a
dev JWKS, then run `gateway_server` pointed at them (`tests/e2e/stack.mjs` does the same thing
for the Playwright suite and is the reference for the environment variables involved:
`ULW_STORAGE`, `ULW_S3_ENDPOINT`, `ULW_BUCKET`, `ULW_S3_ACCESS_KEY_ID`, `ULW_S3_SECRET_ACCESS_KEY`,
`ULW_DATABASE_URL`, `ULW_LISTEN_PORT`, `ULW_DEV_JWKS_FILE`, `JWT_ISSUER`). Once it is listening on
`127.0.0.1`, any of the three scripts above can point `--url` at it.

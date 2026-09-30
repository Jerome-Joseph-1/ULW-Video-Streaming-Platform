# Load tools

Three standalone Python 3 scripts (standard library only, no installs needed) for exercising the
gateway's resumable upload API (`apps/gateway/src/routes.hpp`) under load. They join the existing
C++ load tools in this directory (`gateway_load.cpp`, `loadgen.cpp`).

**These are for the local sandbox and local runs only** — against a `gateway_server` you started
yourself on `127.0.0.1`. Never point them at a real deployment: `slowloris.py` and `flood.py` are
deliberately abusive traffic shapes, and running them against real infrastructure or anyone else's
service would be a denial-of-service attack, not a test.

The gateway limits each client address to 20 connections and 10 new ones a second, and each
user to 300 requests a minute (`docs/integration/uploads.md`). Everything here connects from
one address, so a gateway meant to take load from these tools, rather than to show it refusing
them, needs those raised: `ULW_MAX_CONNECTIONS_PER_IP=448
ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND=65536 ULW_REQUESTS_PER_USER_PER_MINUTE=1000000`.
`slowloris.py` and `flood.py` are the other way round: they run against the limits, and their
legitimate client gets an address of its own with `--legit-source 127.0.0.2` (all of
127.0.0.0/8 is loopback on Linux), as a real client has.

The gateway these run against refuses to start as root unless told which user to become
(`ULW_RUN_AS_USER`) or, for a local run only, `ULW_ALLOW_ROOT=1`.

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

Opens many sockets and trickles bytes into them against the gateway's documented timeouts
(`apps/gateway/src/gateway.hpp`'s `Limits`, enforced in `connection.cpp`): a 10 s header timeout
measured from the request's first byte, a 30 s body idle timeout, and a floor of 8 KiB/s averaged
over each 30 s window a body is read.

```
tests/load/slowloris.py --url http://127.0.0.1:8080 --connections 50 --duration 60 \
    --legit-url http://127.0.0.1:8080 --legit-source 127.0.0.2
```

`--mode headers` (the default) trickles an unfinished header block one byte at a time.
`--mode body` completes a small PATCH's headers, then trickles the body below the 8 KiB/s floor.
`--mode upload` streams a real upload (one 8 MiB store part by default) just above the floor, so
the gateway never cuts it and its part holds a store connection throughout;
`--legit-upload-token` runs an ordinary uploader beside it and reports how long its uploads took.

`--legit-url` runs a concurrent health-check loop (`GET /api/v1/healthz`, optionally with
`--legit-token`) and reports its p50/p99 latency and error rate, to show ordinary traffic isn't
degraded while the slow connections are open. An `https://` URL makes every slow connection
finish its TLS handshake first, so what it trickles is HTTP.

## flood.py

Opens many short-lived connections as fast as it can: unauthenticated requests, a deliberately
oversized header the parser should reject outright, and — with `--token` — authenticated
catalog lookups, all mixed together per worker. It follows `https://` URLs.

```
tests/load/flood.py --url http://127.0.0.1:8080 --duration 30 --rate 500 \
    --legit-url http://127.0.0.1:8080 --legit-source 127.0.0.2 --token "$TOKEN"
```

`--rate` bounds combined requests/s across all workers (0 is unbounded, limited only by
`--workers`). `--legit-url`/`--legit-token` work the same way as in `slowloris.py`.

## call_capacity/call_capacity.py

The call capacity check of brief section 14: 1:1 calls through the pinned LiveKit in a local
container, each participant a LiveKit Python SDK client publishing one 700 kbps track and
receiving its peer's. It reads the SFU container's own interface counters and CPU time and
compares a call's egress with the derivation's 1.4 Mbit/s (brief 8.1, ADR-0012), then reports
the bandwidth ceiling (540 Mbit/s over the measured egress per call) beside the CPU ceiling of
the overlay's 2-core limit. It runs as the `call-capacity` job of `.github/workflows/e2e.yml`.

```
python3 -m venv /tmp/callcap
/tmp/callcap/bin/pip install --require-hashes --only-binary :all: \
    -r tests/load/call_capacity/requirements.txt
sudo /tmp/callcap/bin/python tests/load/call_capacity/call_capacity.py --calls 4 --seconds 60
```

## A minimal local run

Build the gateway and its helpers, start a scratch Postgres database and MinIO bucket, mint a
dev JWKS, then run `gateway_server` pointed at them (`tests/e2e/stack.mjs` does the same thing
for the Playwright suite and is the reference for the environment variables involved:
`ULW_STORAGE`, `ULW_S3_ENDPOINT`, `ULW_BUCKET`, `ULW_S3_ACCESS_KEY_ID`, `ULW_S3_SECRET_ACCESS_KEY`,
`ULW_DATABASE_URL`, `ULW_LISTEN_PORT`, `ULW_DEV_JWKS_FILE`, `JWT_ISSUER`). Once it is listening on
`127.0.0.1`, any of the three scripts above can point `--url` at it.

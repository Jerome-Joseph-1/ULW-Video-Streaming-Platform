# Operations contract

What Askedin's platform provides to the video service, and what the service exposes back for
probes and monitoring. The step-by-step deployment (secrets script lines, database role,
pipeline, rollback) is in [`deploy/askedin/RUNBOOK.md`](../../deploy/askedin/RUNBOOK.md); this
page does not repeat it.

## What the platform provides

| Dependency | Used by | Requirement |
|---|---|---|
| Postgres 16 | gateway, worker, chat | One database, owned by the service's role, so migrations can run DDL (ADR-0031). The gateway's init container (`ulw_migrate`) applies migrations before the gateway starts. |
| Postgres log settings | chat | Bound parameters stay out of the server log: `log_parameter_max_length_on_error = 0` (the default), and `log_parameter_max_length = 0` whenever statement logging is on (`log_statement` `mod` or `all`, `log_min_duration_statement`, `log_min_duration_sample`, `log_transaction_sample_rate`), with `auto_explain.log_parameter_max_length = 0` if auto_explain is loaded. Otherwise chat message bodies, plaintext or ciphertext, are written to the log (ADR-0054). RUNBOOK step 3 sets them on the database. |
| R2 bucket | gateway, worker | One bucket per environment. Lifecycle rule: abort incomplete multipart uploads after 7 days. CORS rule for the app origin, no credentials (ADR-0028, rule text in [videos-and-playback.md](videos-and-playback.md#cors)). |
| R2 API tokens | gateway, worker | One per component (ADR-0066). The gateway's token must allow, on `videos/<video id>/raw` and `videos/<video id>/hls/...`: CreateMultipartUpload, UploadPart, ListParts, CompleteMultipartUpload, AbortMultipartUpload (which must be permitted), HeadObject, GetObject (playlists it rewrites, and presigned GET for segments and init) and PutObject. The upload reaper runs with the gateway's secret and additionally needs ListMultipartUploads (bucket level), ListObjectsV2 and DeleteObject. |
| Askedin JWKS | gateway, chat | Reachable from the pods over HTTPS (`JWKS_URL` must be `https://`). If it is unreachable and no cached key fits a token, requests get `503`, not `401` ([auth.md](auth.md)). |
| DNS and TLS | Envoy | TLS terminates at Askedin's Envoy Gateway; the service speaks plain HTTP behind it (ADR-0001). The HTTPRoute sends `/api/v1/uploads` and `/api/v1/videos` to the gateway. |
| Envoy route timeout | Envoy | None (`request: 0s`). A chunk may take up to 1024 s at the gateway's minimum rate, and the gateway enforces its own timeouts. Upstream idle timeout below the gateway's 10 s keep-alive timeout (5 s in the shipped `BackendTrafficPolicy`). |
| Seccomp profile | worker nodes | `seccomp/ulw-worker.json` installed on the node (RUNBOOK step 2). |
| Envoy routes to LiveKit | Envoy | `/rtc` (the call SDK's WebSocket, no request timeout) and `/whip` (live ingest, RFC 9725; one short request each) to LiveKit's port 7880, on every hostname of `askedin-gateway`: the stage HTTPRoute names none, as the video routes do not. `/twirp` is never routed (ADR-0050, ADR-0053). |
| LiveKit egress and Redis | live streams | Before live streams launch: LiveKit egress v1.14.1 and a Redis that LiveKit and egress both use as their bus. Egress must reach each packager's SRT port (UDP). It uses up to a core and 300 MB per concurrent stream (ADR-0053), and admits a stream only while its configured cost, 2 cores by default, is idle; size it for both. Not in the overlays yet: it ships with the packager's. |

### Environment, by name

<!-- apps/gateway/src/config.cpp, apps/gateway/src/main.cpp, apps/worker/src/config.cpp, apps/worker/src/main.cpp, apps/chat/src/config.cpp, ops/src/settings.cpp -->

The gateway and the worker read each setting from, in rising precedence, its default, a TOML
file (`--config <path>` or `ULW_CONFIG`), the environment, and a command-line flag
(`--listen-port 8081` for `listen.port`: the file key with dots and underscores as dashes). The
Askedin deployment uses the environment only, which is what the table lists. An empty value
counts as unset. A file must be a regular file owned by root or the process's user and not
writable by group or others. Secrets (marked below) are never taken from a flag, and from a file
only if no one but its owner can read it (`0400` or `0600`); the store keys only from the
environment. Chat reads the environment only.

A bad value stops the process at startup with exit code `2` and a `configuration refused` log
line naming the variable; a secret's value is never quoted, not even in the reason a database
URL does not parse. Exit `2` means "fix the configuration, do not just restart": the shipped
systemd units do not restart on it. `gateway_server --check-config` and
`transcode_worker --check-config` run the same checks and exit `0` or `2` without starting
anything. Beyond each value's own range, they check what would otherwise fail only at start: the
connection string parses; the R2 account id or MinIO endpoint forms a store profile; the store
keys are set and the key id is 1 to 128 of `A-Z a-z 0-9 - . _ ~`; the development key set is
allowed (`ULW_DEV_MODE=1`, and not in a Kubernetes pod), reads and holds a usable key; TLS
certificate and key load and match;
`ULW_MAX_UPLOAD_SLOTS <= ULW_MAX_CONNECTIONS`,
`ULW_MAX_UPLOADS_PER_USER <= ULW_MAX_UPLOAD_SLOTS`,
`ULW_MAX_CONNECTIONS_PER_IP <= ULW_MAX_CONNECTIONS`; `ULW_UPLOAD_BYTES_PER_USER_PER_DAY` at
least 16 MiB, the largest `PATCH`; `ULW_TRUSTED_PROXIES` exact CIDR blocks, none of them /0, at
most 16, and `ULW_TRUSTED_PROXY_HOPS` only with them; when the process is root,
`ULW_RUN_AS_USER` names a user or `ULW_ALLOW_ROOT=1` is set; and the descriptor limit covers two
per connection plus 64. `--version` prints the version and commit. At start each logs its
effective configuration, secrets as `<redacted>`.

| Variable | Gateway | Worker | Chat | Notes |
|---|---|---|---|---|
| `ULW_DATABASE_URL` | required | required | required | Secret |
| `ULW_STORAGE` | `r2` (default), `minio`, `fs` | same | | |
| `ULW_R2_ACCOUNT_ID` | with `r2` | with `r2` | | Secret |
| `ULW_S3_ENDPOINT` | with `minio` | with `minio` | | |
| `ULW_BUCKET` | with `r2`/`minio` | same | | Secret |
| `ULW_S3_ACCESS_KEY_ID`, `ULW_S3_SECRET_ACCESS_KEY` | with `r2`/`minio` | same | | Secret, separate tokens per component |
| `JWKS_URL` | required (or `ULW_DEV_JWKS_FILE`) | never set | required (or `ULW_DEV_JWKS_FILE`) | Secret by convention |
| `JWT_ISSUER` | required | never set | required | Secret by convention |
| `JWT_AUDIENCE` | default `askedin-platform` | | same | |
| `ULW_AUTH_COOKIE` | default `auth_token` | | same | `auth_token_stage` on stage |
| `ULW_JWKS_MAX_STALE_HOURS` | 1 to 168, default 24 | | same | How long the keys stay trusted while every JWKS refetch fails; past it every token is refused and `jwks_keys_expired` is `1` ([auth.md](auth.md)) |
| `ULW_DEV_MODE` | `0` (default) or `1` | | same | `1` marks a development run, which `ULW_DEV_JWKS_FILE` needs; that file is refused in a Kubernetes pod whatever this says. Never set in stage or production |
| `ULW_LISTEN_PORT` | default 8080 | | default 9101 | |
| `ULW_TRANSPORT` | `plain` (default) or `tls` | | | `tls` needs `ULW_TLS_CERT_FILE` and `ULW_TLS_KEY_FILE`. Session tickets are sealed with a random in-memory key replaced every 12 h, on a timer, so also on a server no client reaches; the key before it still opens tickets for 12 h more and is then wiped, so a ticket resumes for 12 to 24 h, across certificate reloads, and a leaked key opens at most a day of resumed sessions. Nothing to configure; replicas do not share keys, so a client resumes only on the replica that issued its ticket |
| `ULW_REACTOR` | `io_uring` (default) or `epoll` | | same | Falls back to epoll when io_uring is unavailable |
| `ULW_OFFLOAD_THREADS` | 1 to 64, default 4 | | | |
| `ULW_MAX_CONNECTIONS` | 1 to 65536, default 448 | | | Past this, a new connection is closed at accept |
| `ULW_MAX_UPLOAD_SLOTS` | default 448, at most `ULW_MAX_CONNECTIONS` | | | Chunk uploads in flight at once |
| `ULW_MAX_UPLOADS_PER_USER` | default 3, at most `ULW_MAX_UPLOAD_SLOTS` | | | |
| `ULW_MAX_CONNECTIONS_PER_IP` | default 20, at most `ULW_MAX_CONNECTIONS` | | 1 to 1280, default 20 | Per client address (IPv6: per /64): open connections, or behind a trusted proxy requests (chat: upgrades) in flight until they are authenticated (chat: answered) |
| `ULW_MAX_CONNECTIONS_PER_IP_BLOCK` | | | 1 to 1280, default 4 × `ULW_MAX_CONNECTIONS_PER_IP` (80), at most 1280 | Direct IPv6 peers' open connections per /48, all its /64s together: a customer delegated a /56 or a /48 cannot fill the node from fresh /64s. Past it a new connection is reset at accept (`connections_rejected_total{reason="ip_block"}`, ADR-0076). Below `ULW_MAX_CONNECTIONS_PER_IP` it caps a single /64 too |
| `ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND` | default 10 | | 1 to 65536, default 10 | Direct peers only; burst of the same size |
| `ULW_MAX_SESSIONS_PER_USER` | | | 1 to 1280, default 16 | Open chat sockets per user on a node; past it an upgrade is answered `429` with `Retry-After: 5` (ADR-0076) |
| `ULW_REQUESTS_PER_USER_PER_MINUTE` | default 300 | | | Authenticated requests, burst of the same size |
| `ULW_UPLOAD_BYTES_PER_USER_PER_DAY` | bytes, default 107374182400 (100 GiB), at least 16777216 | | | Charged by each `PATCH`'s `Content-Length`, the part never sent given back; best effort: per replica, in memory, forgotten on restart |
| `ULW_TRUSTED_PROXIES` | comma-separated CIDR blocks, default none | | same | Peers whose `X-Forwarded-For` is believed. Set to the pod network Envoy's data plane runs in (K3s default `10.42.0.0/16`); RUNBOOK step 1. A block shorter than /8 (IPv4) or /32 (IPv6) is logged as a warning. |
| `ULW_TRUSTED_PROXY_HOPS` | 1 to 16, default 1, only with `ULW_TRUSTED_PROXIES` | | same | Proxies in front, each appending one entry: the client is that many entries from the right. Fewer entries, or a malformed one, count the request against the proxy itself. |
| `ULW_RUN_AS_USER` | user name, default none | same | same | Also read by `ulw_reaper` and `ulw_migrate`. Used only when started as root: the process binds its ports and raises its descriptor limit, then becomes this user before it serves, takes a job or dials the database. With `ULW_TRANSPORT=tls` the certificate and key are read after that, at start and on every SIGHUP, so this user must be able to read them. |
| `ULW_ALLOW_ROOT` | `0` (default) or `1` | same | same | Also read by `ulw_reaper` and `ulw_migrate`. Root with no `ULW_RUN_AS_USER` exits `2` unless this is `1`: for development and test harnesses only. |
| `ULW_CHUNK_SIZE` | bytes, default 8388608 (8 MiB) | | | 5 MiB to 5 GiB, and a 50 GiB upload in at most 10,000 chunks |
| `ULW_LOG_LEVEL` | `debug`, `info` (default), `warn`, `error` | same | | |
| `ULW_CONFIG` | optional TOML file | same | | See above |
| `ULW_NODE_ID` | | or `HOSTNAME` | or `HOSTNAME` | RFC 1123 label |
| `ULW_PRESENCE_GRACE_MS` | | | 0 to 600000, default 10000 | How long a user whose last connection closed still shows online ([chat.md](chat.md#presence)) |
| `ULW_SCRATCH_DIR`, `ULW_FFMPEG`, `ULW_FFPROBE`, `ULW_FFMPEG_THREADS`, `ULW_SANDBOX_BIN` | | optional | | |
| `ULW_NODE_ADDRESS`, `ULW_NODE_SECRET`, `ULW_ALLOWED_ORIGINS` | | | required, required (32+ bytes), optional | Chat has no Askedin overlay yet |

The Kubernetes secret names and the lines that create them are in the RUNBOOK, section 3.

<!-- apps/live-packager/src/config.cpp, apps/live-packager/src/main.cpp -->

The live packager (one process per stream, environment only, no Askedin overlay yet) takes
`ULW_STREAM_ID`, `ULW_LIVE_*`, the storage variables above, `ULW_SCRATCH_DIR`, `ULW_FFMPEG` and
`ULW_FFPROBE`. It records an ended stream as a video (ADR-0055) when given both of these, and is
live-only with neither; one without the other stops it at startup:

| Variable | Live packager | Notes |
|---|---|---|
| `ULW_DATABASE_URL` | with recording | Secret; the same database as the gateway's |
| `ULW_STREAM_OWNER` | with recording | The broadcaster's Askedin user id (`sub`), who owns the video |

The role in its database URL needs no more than `SELECT, INSERT` on `live_recordings`, `INSERT`
and `SELECT (id)` on `videos` (the insert returns the id it wrote), `INSERT` on `jobs`, and
`USAGE` on `jobs_id_seq`; its `NOTIFY job_available` needs no grant. Checked against the
migrated schema with a role holding exactly these.

It exits `0` once the stream has ended and its video and job are queued, when the stream was
already recorded, when a newer packager of the stream holds it (`recording: superseded`), when
the stream cannot be recorded at all (`recording: unrecordable: <reason>`, written to
`live_recordings.failure`), and when drained by SIGTERM while the stream is live; and non-zero
otherwise. SIGTERM while it records stops the copy and exits `1`: the stream is not recorded
yet, and the next start records it. Run it with a
restart on failure: a packager killed between the end and the job, or unable to reach the store
or the database then, records the stream on its next start. Started for a stream that has
already ended, it takes no publisher and only records.

While it records it holds one upload part in memory, 16 MiB at the default
`ULW_LIVE_MAX_KBPS` and up to 65 MiB at its 100 Mbit/s ceiling (the part grows with
`ULW_LIVE_MAX_KBPS` times `ULW_LIVE_MAX_HOURS`), beside two copying ffmpeg children of about
60 MB each; its scratch holds one segment. The recording is stored as the video's source,
`videos/<id>/raw`, so the `videos/` rules apply to it: the 7-day abort of incomplete multipart
uploads (row above) and the upload reaper's sweep collect a copy that died midway. The `live/`
prefix needs an expiry of days, not hours: the recording is read back from the segments after
the stream ends.

At the start of each run the live ffmpeg holds its probe window in memory, a segment length
and a second at the publisher's bitrate: 7.5 MB for 3 s at the 20 Mbit/s default ceiling and
about 137 MB at worst, 11 s at 100 Mbit/s (ADR-0057).

## Probes and metrics

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/connection.cpp (advance, readiness_body), apps/gateway/src/health.hpp, apps/gateway/src/health.cpp, apps/gateway/src/gateway.cpp (render_metrics) -->

Gateway, on its HTTP port. None needs a token. The public HTTPRoute does not route them; they
are reachable inside the cluster (the shipped NetworkPolicy admits Envoy and the `monitoring`
namespace).

| Endpoint | Answer |
|---|---|
| `GET /api/v1/healthz` | `200`, `text/plain`, `ok`. The event loop is alive. |
| `GET /healthz` | The same as `GET /api/v1/healthz`, at the path probes conventionally use. |
| `GET /api/v1/readyz` | `200` `ready`, or `503` with the reason as the body: `starting`, `draining`, `database unreachable`, `object store unreachable`, `health probe stuck`. See below. |
| `GET /readyz` | The same as `GET /api/v1/readyz`. |
| `GET /metrics` | `200`, `text/plain; version=0.0.4` (Prometheus text format) |

Readiness checks the dependencies from a thread of its own, never from the event loop, every
5 s: Postgres (the oldest due transcode job's age, on a session of its own with a 2 s
statement timeout) and the store (a read of `health/probe`, a key nothing writes, where "not
found" counts as an answer). Its answers, in order:

- `starting` until the first probe has finished;
- `draining` from SIGTERM on (the probe stops then);
- `health probe stuck` when no probe has finished for 30 s;
- `database unreachable` or `object store unreachable` when that dependency failed two probes
  in a row, or failed its first probe and has never answered. One failure alone is a blip:
  every replica probes the same database and store, and one failure turning them all unready
  together would turn a database failover into an outage.

A dependency that refuses at once is reported within about 10 s; one that hangs holds each
probe until its timeouts, so two misses take up to 2 x (5 s + the probe): about 24 s for a
hanging database, about 36 s when the probe runs to its 13 s worst case. `dependency_up` shows
each probe's raw answer without the two-in-a-row rule.

Gateway metrics. All are counters (`_total`), gauges or histograms, per process:

| Metric | Kind | Meaning |
|---|---|---|
| `build_info{version,git_sha}` | gauge | Always 1; the labels say what is running |
| `ready` | gauge | 1 while `/readyz` answers `200` |
| `dependency_up{dependency="database"}`, `{dependency="store"}` | gauge | The last probe's raw answer |
| `requests_total` | counter | Requests parsed |
| `responses_total{class="1xx"}` ... `{class="5xx"}` | counter | Responses sent, by status class |
| `connections_accepted_total` | counter | |
| `connections_rejected_total{reason="capacity"}` | counter | Refused at accept: `ULW_MAX_CONNECTIONS` (448) were open |
| `connections_rejected_total{reason="socket"}` | counter | Refused at accept: the socket could not be set up |
| `connections_rejected_total{reason="draining"}` | counter | Accepted after SIGTERM and closed |
| `connections_rejected_total{reason="ip_connections"}` | counter | Reset at accept: the address had `ULW_MAX_CONNECTIONS_PER_IP` open |
| `connections_rejected_total{reason="ip_rate"}` | counter | Reset at accept: the address opened more than `ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND` |
| `connections_current` | gauge | |
| `uploads_in_flight` | gauge | Chunk uploads holding an admission slot |
| `admission_rejections_total` | counter | PATCHes answered `429` or `503` by admission |
| `rate_limited_total{limit="ip_requests"}` | counter | `429`: a client behind the proxy had `ULW_MAX_CONNECTIONS_PER_IP` unauthenticated requests in flight |
| `rate_limited_total{limit="user_requests"}` | counter | `429`: a user over `ULW_REQUESTS_PER_USER_PER_MINUTE` |
| `rate_limited_total{limit="user_bytes"}` | counter | `429`: a `PATCH` over the user's `ULW_UPLOAD_BYTES_PER_USER_PER_DAY` |
| `rate_limit_entries{table="client"}`, `{table="user"}` | gauge | Client addresses and users the limits remember; at most 16384 each (more clients when `ULW_MAX_CONNECTIONS` is higher) |
| `rate_limit_evictions_total{table="client"}`, `{table="user"}` | counter | Entries forgotten to make room: the least recently seen, never one with a connection or request open. A forgotten user starts over with full allowances. |
| `bytes_ingested_total` | counter | Chunk body bytes received |
| `part_upload_duration_seconds` | histogram | From a chunk's first byte handed to the store to all of it durable |
| `backend_write_stall_seconds` | histogram | Each wait of a chunk body on a store that took nothing more, observed when it ends: the store takes bytes again, fails the part (`503`), or the request ends (backstop, client gone). Buckets to 300 s; a store taking nothing is failed at about 60 s (ADR-0045) |
| `buffer_bytes_in_use` | gauge | Bytes held in connections' staging and body buffers |
| `timeouts_total{kind="header"}` | counter | Request head not complete within 10 s, or an idle keep-alive closed, 10 s after its last response was queued (a new request from a client whose last response is still held back by its window waits unread, and does not restart the count). The close is a reset if part of the response had not yet reached the kernel; otherwise a FIN, after which the kernel finishes the response for at most 20 s. The same holds for the 2 s linger after a `Connection: close` response, and for a drain, which lingers on a connection still reading its response (ADR-0071) |
| `timeouts_total{kind="body"}` | counter | Body idle 30 s (`408`) |
| `timeouts_total{kind="body_rate"}` | counter | Body under 8 KiB/s over a 30 s window (`408`) |
| `timeouts_total{kind="backstop"}` | counter | Request older than 6 h, closed |
| `tls_handshakes_in_flight` | gauge | Only with `ULW_TRANSPORT=tls` |
| `tls_handshake_failures_total` | counter | |
| `certificate_reloads_total`, `certificate_reload_failures_total` | counter | SIGHUP certificate reloads |
| `playlist_requests_total{kind="master"}`, `{kind="media"}`, `{kind="live"}` | counter | |
| `live_playlist_cache_hits_total` | counter | Live playlist requests answered from the cache: a fresh copy, or a stream remembered as absent for 1 s (ADR-0059) |
| `live_playlist_cache_misses_total` | counter | Live playlist requests that found no fresh copy; each started a store read or joined one |
| `live_playlist_fetches_total` | counter | Live playlists read from the store. At most one per stream per half target duration, whatever the audience |
| `live_playlist_single_flight_joins_total` | counter | Misses that waited on a store read another request had started instead of starting one |
| `live_playlist_cache_evictions_total` | counter | Fresh copies dropped for the bounds (512 streams, 4 MiB) |
| `live_playlist_cache_entries`, `live_playlist_cache_bytes` | gauge | Streams and bytes held |
| `playlists_rejected_total` | counter | A stored playlist broke a rewriting rule (a worker bug); the viewer got `500` |
| `presign_failures_total` | counter | A segment URL could not be signed; the viewer got `500` |
| `view_events_recorded_total`, `view_events_dropped_total`, `view_batches_failed_total` | counter | Master-playlist fetches recorded as views |
| `jobs_oldest_queued_seconds` | gauge | How long the oldest transcode job due to run has waited; `0` when none waits, `NaN` while the database does not answer |
| `jwks_keys_expired` | gauge | `1` while every token is refused because the JWKS went unrefreshed for `ULW_JWKS_MAX_STALE_HOURS` ([auth.md](auth.md)) |
| `store_paging_errors_total` | counter | Store failures only a fix on our side cures: signature, credentials, bucket |
| `log_messages_dropped_total` | counter | Log lines dropped because the log reader fell behind |
| `open_fds` | gauge | Descriptors open in the process |
| `resident_memory_bytes` | gauge | Resident set size of the process |

Worth alerting on: `readyz` failing outside a rollout; `jwks_keys_expired` at `1` (page: no
token verifies until Askedin's JWKS is reachable again); any rise in `playlists_rejected_total`,
`presign_failures_total`, `view_batches_failed_total` or `store_paging_errors_total` (page:
retrying will not fix it); `admission_rejections_total` rising steadily;
`backend_write_stall_seconds` observations at 30 s and above rising (the bucket is slow);
`jobs_oldest_queued_seconds` growing (the workers are behind or down);
`log_messages_dropped_total` rising; `connections_rejected_total{reason="ip_connections"}` or
`{reason="ip_rate"}` rising steadily behind Envoy (`ULW_TRUSTED_PROXIES` does not cover Envoy's
pods, so every client is being counted as Envoy); `rate_limit_evictions_total{table="user"}`
rising (more active users than the table remembers, so allowances are being reset);
`live_playlist_fetches_total` rising faster than two per live stream per target duration (the
cache is not absorbing reloads: check `live_playlist_cache_evictions_total` for a budget too
small for the streams being watched).

### Logs

<!-- ops/include/ops/log.hpp, ops/src/async_log.cpp, apps/gateway/src/main.cpp -->

The gateway and the worker write one JSON object per line to stdout:
`{"ts":...,"level":...,"svc":...,"event":...,<fields>}`, `level` one of `debug`, `info`,
`warn`, `error`. A line is at most 1 KiB; a longer one is cut and marked `"truncated":true`.
Tokens, keys, signed URLs, request targets, headers and bodies are never logged. The gateway
writes one `request` line per response (request id, method, route name, status, milliseconds,
body length): `info`, `warn` for a 5xx, `debug` for probes and scrapes.

The gateway never waits for its log reader: lines go to a 256 KiB buffer that a thread of its
own writes out. When the reader falls behind and the buffer is full, lines are dropped and
counted in `log_messages_dropped_total`. At exit it flushes for at most 2 s and, if any line was
ever dropped, writes one `log lines dropped` line with the total to stderr. The worker writes
each line directly.

Worker: no HTTP port. Liveness is a heartbeat file, `<ULW_SCRATCH_DIR>/heartbeat-<node>`,
touched at least every 20 s; the shipped probe restarts the pod after two minutes without a
touch. The worker exposes no metrics yet.

<!-- apps/chat/src/session.cpp (route), apps/chat/src/chat.cpp (render_metrics) -->

Chat, on its client port (default 9101): `GET /healthz` (loop alive), `GET /readyz` (not
draining, node address published, owner heartbeat reaching the database), `GET /metrics`, with
`connections_accepted_total`, `connections_rejected_total{reason="capacity"}` (closed at accept:
1280 sessions were open), `connections_rejected_total{reason="socket"}` (closed at accept, as the
gateway counts it: the peer address could not be read, a socket already gone or not an IP one,
or the socket refused its options or the reactor would not take it),
`connections_rejected_total{reason="ip_connections"}`, `{reason="ip_block"}` and
`{reason="ip_rate"}` (direct peers reset at accept: the address's open connections, its IPv6
/48's, or its new connections a second), `upgrades_limited_total{limit="ip"}` and
`{limit="user_sessions"}` (upgrades answered `429`), `rate_limit_entries{table="client"}`,
`{table="user"}` and `{table="ip_block"}`, `rate_limit_evictions_total{table="client"}`
(ADR-0076),
`connections_current`, `websocket_upgrades_total`, `auth_failures_total`,
`origin_rejections_total`, `messages_received_total`, `messages_delivered_total`,
`messages_rate_limited_total`, `messages_deduplicated_total`, `lossy_drops_total`,
`messages_replayed_total`, `history_messages_total`, `messages_kept_bytes`,
`protocol_errors_total`, `control_floods_total`, `slow_consumers_total`, `stalled_readers_total`,
`allocation_failures_total`, `rooms_active`, `rooms_joined`, `room_reassignments_total`,
`fenced_writes_total`, `forwards_total`, `forward_timeouts_total`, `peers_lost_total`,
`peers_refused_total`, `slow_peers_total`, `presence_rooms`, `presence_events_sent_total`,
`presence_events_received_total`, `presence_notifications_total`, `token_expiries_total`
(sockets closed with 4001 as their token ran out), `member_removals_total` (sockets taken out of
a room because their user left its member list, ADR-0073), `member_check_failures_total`
(member checks after a lost listening session that failed other than for an unreachable
database, each settled by taking the user's sockets out of the room with `unavailable`, and
logged), `presence_expired_total`
(announcements and watching nodes dropped because they stopped being renewed, normally a node
that died), `presence_gaps_total` (seqs a presence room skipped at this node, after which the
node repeated what it had said there), `jwks_keys_expired` (as the gateway's),
`unrecorded_joins_total` (refused joins of rooms with no kind recorded that recorded nothing,
their user past the allowance: steady growth is someone walking room ids). Chat is a draft
([chat.md](chat.md)).
`lossy_drops_total` counts messages lossy clients (every viewer of a stream's live chat) were
moved past because they were behind (ADR-0070): a node whose count climbs has viewers that
cannot keep up, not a fault of its own. Each chat connection's kernel send buffer is fixed at
64 KiB, so chat's pod memory is bounded at about 820 MiB of its 1 GiB, kernel buffers included.
`slow_peers_total` counts node-channel connections reset because the other node stopped
reading: about 1 MiB queued for it, or 20 s with output waiting and none of it acknowledged, which
a node that vanished also shows (ADR-0071). A node that is only busy, reading a little at a time,
keeps its link.

## Shutdown

On SIGTERM the gateway stops accepting, answers `readyz` with `503`, lets requests in flight
finish for up to 30 s, then cuts off what remains. A client whose chunk was cut off resumes from
`HEAD` ([uploads.md](uploads.md#resuming)). Give the pod a termination grace period above 30 s
(the shipped Deployment uses 45 s): the drain's 30 s, the health probe finishing (it stops when
the drain begins) and the 2 s log flush fit inside it.

## Core dumps

The gateway, chat server, reaper, worker, live packager and `ulw_migrate` write no core file and
are not dumpable: each sets `RLIMIT_CORE` to 0 (soft and hard) and `PR_SET_DUMPABLE` to 0 before
it reads its configuration, and keeps the flag off across its drop from root. Their memory holds
the database password, the store keys and live bearer tokens. A crash is diagnosed from the log;
`/proc/<pid>/environ` and ptrace are closed to other processes of the same user, root aside.
When `kernel.core_pattern` is a pipe (`|/usr/lib/systemd/systemd-coredump ...`, apport), the
kernel ignores an `RLIMIT_CORE` of 0 and hands the core to the helper anyway; the services are
still covered, because a process that is not dumpable is not dumped through a pipe either.
The ffmpeg sandbox sets its own `RLIMIT_CORE` 0 as before, and exec resets the dumpable flag for
the sandboxed child, whose seccomp filter is unchanged.

# Operations contract

What Askedin's platform provides to the video service, and what the service exposes back for
probes and monitoring. The step-by-step deployment (secrets script lines, database role,
pipeline, rollback) is in [`deploy/askedin/RUNBOOK.md`](../../deploy/askedin/RUNBOOK.md); this
page does not repeat it.

## What the platform provides

| Dependency | Used by | Requirement |
|---|---|---|
| Postgres 16 | gateway, worker, chat | One database, owned by the service's role, so migrations can run DDL (ADR-0031). The gateway's init container (`ulw_migrate`) applies migrations before the gateway starts. |
| Postgres log settings | chat | Bound parameters stay out of the server log: `log_parameter_max_length_on_error = 0` (the default), and `log_parameter_max_length = 0` whenever statement logging is on (`log_statement` `mod` or `all`, `log_min_duration_statement`, `log_min_duration_sample`, `log_transaction_sample_rate`), with `auto_explain.log_parameter_max_length = 0` if auto_explain is loaded. Otherwise chat message bodies, plaintext or ciphertext, are written to the log (ADR-0052). RUNBOOK step 3 sets them on the database. |
| R2 bucket | gateway, worker | One bucket per environment. Lifecycle rule: abort incomplete multipart uploads after 7 days. CORS rule for the app origin, no credentials (ADR-0028, rule text in [videos-and-playback.md](videos-and-playback.md#cors)). |
| R2 API tokens | gateway, worker | One per component, object read and write on the bucket. The gateway's token must also allow multipart create, upload part, list parts, complete and abort, and presigned GET. |
| Askedin JWKS | gateway, chat | Reachable from the pods over HTTPS (`JWKS_URL` must be `https://`). If it is unreachable and no cached key fits a token, requests get `503`, not `401` ([auth.md](auth.md)). |
| DNS and TLS | Envoy | TLS terminates at Askedin's Envoy Gateway; the service speaks plain HTTP behind it (ADR-0001). The HTTPRoute sends `/api/v1/uploads` and `/api/v1/videos` to the gateway. |
| Envoy route timeout | Envoy | None (`request: 0s`). A chunk may take up to 1024 s at the gateway's minimum rate, and the gateway enforces its own timeouts. Upstream idle timeout below the gateway's 10 s keep-alive timeout (5 s in the shipped `BackendTrafficPolicy`). |
| Seccomp profile | worker nodes | `seccomp/ulw-worker.json` installed on the node (RUNBOOK step 2). |

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
URL does not parse.
Exit `2` means "fix the configuration, do not just restart": the shipped systemd units do not
restart on it. `gateway_server --check-config` and `transcode_worker --check-config` run the
same checks and exit `0` or `2` without starting anything. Beyond each value's own range, they
check what would otherwise fail only at start: the connection string parses; the R2 account
id or MinIO endpoint forms a store profile; the store keys are set and the key id is 1 to 128
of `A-Z a-z 0-9 - . _ ~`; the development key set reads and holds a usable key; TLS certificate
and key load and match; `ULW_MAX_UPLOAD_SLOTS <= ULW_MAX_CONNECTIONS`,
`ULW_MAX_UPLOADS_PER_USER <= ULW_MAX_UPLOAD_SLOTS`; and the descriptor limit covers two per
connection plus 64. `--version` prints the version and commit. At start each logs its
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
| `ULW_LISTEN_PORT` | default 8080 | | default 9101 | |
| `ULW_TRANSPORT` | `plain` (default) or `tls` | | | `tls` needs `ULW_TLS_CERT_FILE` and `ULW_TLS_KEY_FILE` |
| `ULW_REACTOR` | `io_uring` (default) or `epoll` | | same | Falls back to epoll when io_uring is unavailable |
| `ULW_OFFLOAD_THREADS` | 1 to 64, default 4 | | | |
| `ULW_MAX_CONNECTIONS` | 1 to 65536, default 448 | | | Past this, a new connection is closed at accept |
| `ULW_MAX_UPLOAD_SLOTS` | default 448, at most `ULW_MAX_CONNECTIONS` | | | Chunk uploads in flight at once |
| `ULW_MAX_UPLOADS_PER_USER` | default 3, at most `ULW_MAX_UPLOAD_SLOTS` | | | |
| `ULW_CHUNK_SIZE` | bytes, default 8388608 (8 MiB) | | | 5 MiB to 5 GiB, and a 50 GiB upload in at most 10,000 chunks |
| `ULW_LOG_LEVEL` | `debug`, `info` (default), `warn`, `error` | same | | |
| `ULW_CONFIG` | optional TOML file | same | | See above |
| `ULW_NODE_ID` | | or `HOSTNAME` | or `HOSTNAME` | RFC 1123 label |
| `ULW_SCRATCH_DIR`, `ULW_FFMPEG`, `ULW_FFPROBE`, `ULW_FFMPEG_THREADS`, `ULW_SANDBOX_BIN` | | optional | | |
| `ULW_NODE_ADDRESS`, `ULW_NODE_SECRET`, `ULW_ALLOWED_ORIGINS` | | | required, required (32+ bytes), optional | Chat has no Askedin overlay yet |

The Kubernetes secret names and the lines that create them are in the RUNBOOK, section 3.

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
| `connections_current` | gauge | |
| `uploads_in_flight` | gauge | Chunk uploads holding an admission slot |
| `admission_rejections_total` | counter | PATCHes answered `429` or `503` by admission |
| `bytes_ingested_total` | counter | Chunk body bytes received |
| `part_upload_duration_seconds` | histogram | From a chunk's first byte handed to the store to all of it durable |
| `backend_write_stall_seconds` | histogram | Each wait of a chunk body on a store that took nothing more |
| `buffer_bytes_in_use` | gauge | Bytes held in connections' staging and body buffers |
| `timeouts_total{kind="header"}` | counter | Request head not complete within 10 s, or an idle keep-alive closed |
| `timeouts_total{kind="body"}` | counter | Body idle 30 s (`408`) |
| `timeouts_total{kind="body_rate"}` | counter | Body under 8 KiB/s over a 30 s window (`408`) |
| `timeouts_total{kind="backend"}` | counter | The store took nothing for 30 s (`503`) |
| `timeouts_total{kind="backstop"}` | counter | Request older than 6 h, closed |
| `tls_handshakes_in_flight` | gauge | Only with `ULW_TRANSPORT=tls` |
| `tls_handshake_failures_total` | counter | |
| `certificate_reloads_total`, `certificate_reload_failures_total` | counter | SIGHUP certificate reloads |
| `playlist_requests_total{kind="master"}`, `{kind="media"}` | counter | |
| `playlists_rejected_total` | counter | A stored playlist broke a rewriting rule (a worker bug); the viewer got `500` |
| `presign_failures_total` | counter | A segment URL could not be signed; the viewer got `500` |
| `view_events_recorded_total`, `view_events_dropped_total`, `view_batches_failed_total` | counter | Master-playlist fetches recorded as views |
| `jobs_oldest_queued_seconds` | gauge | How long the oldest transcode job due to run has waited; `0` when none waits, `NaN` while the database does not answer |
| `store_paging_errors_total` | counter | Store failures only a fix on our side cures: signature, credentials, bucket |
| `log_messages_dropped_total` | counter | Log lines dropped because the log reader fell behind |
| `open_fds` | gauge | Descriptors open in the process |
| `resident_memory_bytes` | gauge | Resident set size of the process |

Worth alerting on: `readyz` failing outside a rollout; any rise in `playlists_rejected_total`,
`presign_failures_total`, `view_batches_failed_total` or `store_paging_errors_total` (page:
retrying will not fix it); `admission_rejections_total` rising steadily;
`timeouts_total{kind="backend"}` rising (the bucket is slow); `jobs_oldest_queued_seconds`
growing (the workers are behind or down); `log_messages_dropped_total` rising.

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
`connections_accepted_total`, `connections_rejected_total{reason="capacity"}`,
`connections_current`, `websocket_upgrades_total`, `auth_failures_total`,
`origin_rejections_total`, `messages_received_total`, `messages_delivered_total`,
`messages_rate_limited_total`, `messages_deduplicated_total`, `lossy_drops_total`,
`messages_replayed_total`, `history_messages_total`, `messages_kept_bytes`,
`protocol_errors_total`, `control_floods_total`, `slow_consumers_total`,
`allocation_failures_total`, `rooms_active`, `rooms_joined`, `room_reassignments_total`,
`fenced_writes_total`, `forwards_total`, `forward_timeouts_total`, `peers_lost_total`,
`peers_refused_total`, `slow_peers_total`. Chat is a draft ([chat.md](chat.md)).

## Shutdown

On SIGTERM the gateway stops accepting, answers `readyz` with `503`, lets requests in flight
finish for up to 30 s, then cuts off what remains. A client whose chunk was cut off resumes from
`HEAD` ([uploads.md](uploads.md#resuming)). Give the pod a termination grace period above 30 s (the shipped Deployment uses 45 s): the drain's 30 s, the health probe finishing (it stops when the drain begins) and the 2 s log flush fit inside it.

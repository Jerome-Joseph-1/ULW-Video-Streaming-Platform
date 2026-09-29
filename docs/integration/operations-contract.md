# Operations contract

What Askedin's platform provides to the video service, and what the service exposes back for
probes and monitoring. The step-by-step deployment (secrets script lines, database role,
pipeline, rollback) is in [`deploy/askedin/RUNBOOK.md`](../../deploy/askedin/RUNBOOK.md); this
page does not repeat it.

## What the platform provides

| Dependency | Used by | Requirement |
|---|---|---|
| Postgres 16 | gateway, worker | One database, owned by the service's role, so migrations can run DDL (ADR-0031). The gateway's init container (`ulw_migrate`) applies migrations before the gateway starts. |
| Postgres log settings | chat | Bound parameters stay out of the server log: `log_parameter_max_length_on_error = 0` (the default), and `log_parameter_max_length = 0` whenever statement logging is on (`log_statement` `mod` or `all`, `log_min_duration_statement`, `log_min_duration_sample`, `log_transaction_sample_rate`), with `auto_explain.log_parameter_max_length = 0` if auto_explain is loaded. Otherwise chat message bodies, plaintext or ciphertext, are written to the log (ADR-0039). RUNBOOK step 3 sets them on the database. |
| R2 bucket | gateway, worker | One bucket per environment. Lifecycle rule: abort incomplete multipart uploads after 7 days. CORS rule for the app origin, no credentials (ADR-0028, rule text in [videos-and-playback.md](videos-and-playback.md#cors)). |
| R2 API tokens | gateway, worker | One per component, object read and write on the bucket. The gateway's token must also allow multipart create, upload part, list parts, complete and abort, and presigned GET. |
| Askedin JWKS | gateway, chat | Reachable from the pods over HTTPS (`JWKS_URL` must be `https://`). If it is unreachable and no cached key fits a token, requests get `503`, not `401` ([auth.md](auth.md)). |
| DNS and TLS | Envoy | TLS terminates at Askedin's Envoy Gateway; the service speaks plain HTTP behind it (ADR-0001). The HTTPRoute sends `/api/v1/uploads` and `/api/v1/videos` to the gateway. |
| Envoy route timeout | Envoy | None (`request: 0s`). A chunk may take up to 1024 s at the gateway's minimum rate, and the gateway enforces its own timeouts. Upstream idle timeout below the gateway's 10 s keep-alive timeout (5 s in the shipped `BackendTrafficPolicy`). |
| Seccomp profile | worker nodes | `seccomp/ulw-worker.json` installed on the node (RUNBOOK step 2). |

### Environment, by name

<!-- apps/gateway/src/config.cpp, apps/gateway/src/main.cpp, apps/worker/src/config.cpp, apps/chat/src/config.cpp -->

Every setting comes from the environment; nothing is read from argv. An empty value counts as
unset. A bad value stops the process at startup with a message naming the variable.

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
| `ULW_NODE_ID` | | or `HOSTNAME` | or `HOSTNAME` | RFC 1123 label |
| `ULW_SCRATCH_DIR`, `ULW_FFMPEG`, `ULW_FFPROBE`, `ULW_FFMPEG_THREADS`, `ULW_SANDBOX_BIN` | | optional | | |
| `ULW_NODE_ADDRESS`, `ULW_NODE_SECRET`, `ULW_ALLOWED_ORIGINS` | | | required, required (32+ bytes), optional | Chat has no Askedin overlay yet |

The Kubernetes secret names and the lines that create them are in the RUNBOOK, section 3.

## Probes and metrics

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/connection.cpp (advance), apps/gateway/src/gateway.cpp (render_metrics) -->

Gateway, on its HTTP port. None needs a token. The public HTTPRoute does not route them; they
are reachable inside the cluster (the shipped NetworkPolicy admits Envoy and the `monitoring`
namespace).

| Endpoint | Answer |
|---|---|
| `GET /api/v1/healthz` | `200`, `text/plain`, `ok`. The event loop is alive. |
| `GET /api/v1/readyz` | `200` `ready`, or `503` `draining` once shutdown has begun (SIGTERM). It does not check Postgres or the bucket. |
| `GET /metrics` | `200`, `text/plain; version=0.0.4` (Prometheus text format) |

Gateway metrics. All are counters (`_total`) or gauges, per process:

| Metric | Kind | Meaning |
|---|---|---|
| `requests_total` | counter | Requests parsed |
| `connections_accepted_total` | counter | |
| `connections_rejected_total{reason="capacity"}` | counter | Refused at accept: the 448-connection table was full, or the socket could not be set up |
| `connections_current` | gauge | |
| `uploads_in_flight` | gauge | Chunk uploads holding an admission slot |
| `admission_rejections_total` | counter | PATCHes answered `429` or `503` by admission |
| `bytes_ingested_total` | counter | Chunk body bytes received |
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

Worth alerting on: `readyz` failing outside a rollout; any rise in `playlists_rejected_total`,
`presign_failures_total` or `view_batches_failed_total`; `admission_rejections_total` rising
steadily; `timeouts_total{kind="backend"}` rising (the bucket is slow).

Worker: no HTTP port. Liveness is a heartbeat file, `<ULW_SCRATCH_DIR>/heartbeat-<node>`,
touched at least every 20 s; the shipped probe restarts the pod after two minutes without a
touch. The worker exposes no metrics yet.

<!-- apps/chat/src/session.cpp (route), apps/chat/src/chat.cpp (render_metrics) -->

Chat, on its client port (default 9101): `GET /healthz` (loop alive), `GET /readyz` (not
draining, node address published, owner heartbeat reaching the database), `GET /metrics`, with
`connections_accepted_total`, `connections_rejected_total{reason="capacity"}`,
`connections_current`, `websocket_upgrades_total`, `auth_failures_total`,
`origin_rejections_total`, `messages_received_total`, `messages_delivered_total`,
`protocol_errors_total`, `control_floods_total`, `slow_consumers_total`,
`allocation_failures_total`, `rooms_active`, `rooms_joined`, `room_reassignments_total`,
`fenced_writes_total`, `forwards_total`, `forward_timeouts_total`, `peers_lost_total`,
`peers_refused_total`, `slow_peers_total`. Chat is a draft ([chat.md](chat.md)).

## Shutdown

On SIGTERM the gateway stops accepting, answers `readyz` with `503`, lets requests in flight
finish for up to 30 s, then cuts off what remains. A client whose chunk was cut off resumes from
`HEAD` ([uploads.md](uploads.md#resuming)). Give the pod a termination grace period above 30 s (the shipped Deployment uses 45 s).

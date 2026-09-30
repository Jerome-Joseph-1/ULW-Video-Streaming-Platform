# Architecture decisions

Decisions, one per file, immutable; supersede rather than edit.

| # | Decision | Status |
|---|---|---|
| [0001](0001-tls-in-process-behind-a-transport-port.md) | TLS in-process behind a transport port | Accepted |
| [0002](0002-vod-playback-never-proxies-segment-bytes.md) | VOD playback never proxies segment bytes | Accepted |
| [0003](0003-epoll-readiness-reactor.md) | An epoll readiness reactor as the event loop | Superseded by 0010 |
| [0004](0004-storage-port-at-the-multipart-part-level.md) | Storage port at the multipart part level | Superseded by 0009 |
| [0005](0005-unsigned-payload-for-upload-part.md) | UNSIGNED-PAYLOAD for UploadPart, over TLS only | Accepted |
| [0006](0006-postgres-job-queue.md) | The job queue is a Postgres table | Accepted |
| [0007](0007-admission-control-from-the-memory-budget.md) | Admission control per route, derived from the memory budget | Superseded by 0027 |
| [0008](0008-expected-for-recoverable-errors.md) | std::expected for recoverable errors, exceptions for bugs | Accepted |
| [0009](0009-storage-port-at-the-durable-offset.md) | Storage is abstracted at the durable offset, not the part | Accepted |
| [0010](0010-io-uring-primary-reactor.md) | io_uring is the primary reactor, epoll the fallback | Accepted |
| [0011](0011-cloudflare-r2-object-storage.md) | Cloudflare R2 for production object storage | Accepted |
| [0012](0012-realtime-media-is-a-separate-tier.md) | Realtime media is a separate tier that proxies bytes | Accepted |
| [0013](0013-webrtc-ingress-through-stunner.md) | WebRTC media ingresses through STUNner | Accepted |
| [0014](0014-live-viewers-get-hls-from-r2.md) | Live-stream viewers get HLS from R2, not WebRTC fan-out | Accepted |
| [0015](0015-single-owner-rooms-with-fenced-writes.md) | One owning node per room, with generation-fenced writes | Accepted |
| [0016](0016-end-to-end-encryption-for-private-chat-only.md) | End-to-end encryption for private chat only | Accepted |
| [0017](0017-sans-io-protocol-codecs.md) | Protocol codecs are sans-IO | Accepted |
| [0018](0018-identity-from-askedin-jwts.md) | Identity is borrowed from Askedin | Accepted |
| [0019](0019-chat-in-its-own-binary.md) | Chat runs in its own binary | Accepted |
| [0020](0020-livekit-as-the-first-sfu.md) | LiveKit as the first SFU | Accepted |
| [0021](0021-single-shot-buffer-select-recv.md) | Single-shot buffer-select receives, not multishot | Accepted |
| [0022](0022-clang-19-minimum.md) | Clang 19 is the minimum Clang | Accepted |
| [0023](0023-uuidv7-identifiers.md) | Identifiers are UUIDv7 | Accepted |
| [0024](0024-playlists-served-inline-with-presigned-uris.md) | Playlists are rewritten and served inline | Accepted |
| [0025](0025-ffmpeg-as-a-sandboxed-subprocess.md) | FFmpeg runs as a sandboxed subprocess | Accepted |
| [0026](0026-ordinary-descriptors-for-accepted-sockets.md) | Accepted sockets use ordinary descriptors | Accepted |
| [0027](0027-admission-control-from-measured-connection-memory.md) | Admission control from the measured memory of a connection | Accepted |
| [0028](0028-segments-fetched-cross-origin-without-credentials.md) | Viewers fetch segments cross-origin, without credentials | Accepted |
| [0029](0029-websocket-messages-own-their-payload.md) | WebSocket messages own their payload | Accepted |
| [0030](0030-container-images-on-ubuntu-from-a-dated-snapshot.md) | Container images build and run on Ubuntu 24.04 from a dated snapshot | Accepted |
| [0031](0031-schema-migrations-in-the-gateways-init-container.md) | Schema migrations run in the gateway's init container | Accepted |
| [0032](0032-worker-pods-in-their-own-user-namespace.md) | The worker pod runs in its own user namespace with a derived seccomp profile | Accepted |
| [0033](0033-sdp-parsed-strictly-and-serialized-exactly.md) | SDP is parsed strictly into a typed model that serializes back exactly | Accepted |
| [0034](0034-rtp-and-rtcp-read-in-place.md) | RTP and RTCP are read in place, for tooling and tests | Accepted |
| [0035](0035-node-channel-over-framed-tcp.md) | The node channel is framed TCP on the reactor | Accepted |
| [0036](0036-chat-server-client-edge.md) | chat_server's client edge: envelope, limits and allocation failure | Accepted |
| [0037](0037-turn-credentials-minted-by-the-sfu.md) | STUNner runs in front of LiveKit with time-windowed credentials the SFU mints | Accepted |
| [0038](0038-single-use-key-packages-in-postgres.md) | Single-use key packages in Postgres, with a replenish signal | Accepted |
| [0039](0039-json-logs-metrics-and-readiness-off-the-loop.md) | JSON logs, metrics and readiness, none of them waiting on the loop | Accepted |
| [0040](0040-layered-configuration-with-a-toml-subset.md) | Layered configuration from a TOML subset, the environment and flags | Accepted |
| [0041](0041-a-job-result-the-queue-refuses-fails-the-job.md) | A job result the queue refuses fails the job | Accepted |
| [0042](0042-soak-flatness-from-the-memory-limit-and-requests-in-flight.md) | Soak flatness judged per unit of work against the memory limit | Accepted |
| [0043](0043-chat-service-policy-between-edge-and-rooms.md) | The chat service: policy between the client edge and the room plane | Accepted |
| [0044](0044-openmls-behind-a-narrow-c-api.md) | OpenMLS behind a narrow C API, pinned by Cargo.lock | Accepted |
| [0045](0045-one-store-connection-per-admitted-upload.md) | One store connection per admitted upload, and a held body is the store's to end | Accepted |
| [0046](0046-live-media-reaches-the-packager-over-srt.md) | Live media reaches the packager over SRT, which it terminates itself | Accepted, probe window amended by 0057 |
| [0047](0047-live-playlist-window-and-restart.md) | The live playlist is a window the packager owns, and it continues across a restart | Accepted |
| [0048](0048-seccomp-allowlist-for-the-ffmpeg-child.md) | The ffmpeg child runs under a syscall allowlist | Accepted |
| [0049](0049-upload-reaper-as-a-cron-job.md) | Abandoned uploads are reaped by a CronJob | Accepted |
| [0050](0050-clients-reach-the-sfu-with-a-ticket.md) | Clients reach the SFU with a ticket, not through our signalling | Accepted |
| [0051](0051-datagram-mode-in-the-reactor.md) | Datagram mode in the reactor | Accepted |
| [0052](0052-per-client-limits-refused-before-the-handshake.md) | Per-client limits, refused before the handshake, and dropping root | Accepted |
| [0053](0053-live-ingest-over-whip-straight-to-the-sfu.md) | Live ingest is WHIP straight to the SFU, relayed to the packager by its recorder | Accepted |
| [0054](0054-messages-stored-with-their-seq.md) | A message is stored with its seq, in one statement, before it is delivered | Accepted |
| [0055](0055-a-live-recording-is-remuxed-from-the-stored-segments.md) | A live recording is remuxed from its stored segments into one object, and queued once per stream | Accepted |
| [0056](0056-presence-over-the-room-plane.md) | Presence over the room plane, with a grace and a lease | Accepted |
| [0057](0057-the-probe-window-follows-the-configuration.md) | The packager's probe window follows its configuration | Accepted |
| [0058](0058-group-calls-interfaces-now-implementation-deferred.md) | Group calls: interfaces now, implementation deferred | Accepted |
| [0059](0059-live-playlists-through-a-single-flight-cache.md) | Live playlists are served from a single-flight cache, to any signed-in viewer | Accepted |
| [0060](0060-pump-staging-is-appended-to-and-bounded.md) | Pump staging is appended to, bounded, and overflow is 413 | Accepted |
| [0061](0061-cmake-3-28-presets-v6-and-the-ci-test-preset.md) | CMake 3.28, presets version 6, and a `ci` test preset | Accepted |
| [0062](0062-ulw-sanitize-is-the-one-sanitizer-target.md) | `ulw_sanitize` is the one sanitizer interface target | Accepted |
| [0063](0063-gateway-memory-high-600m-max-700m.md) | The gateway's memory: high at 600 MB, max at 700 MB | Accepted |
| [0064](0064-the-first-accepted-patch-starts-the-upload.md) | The first accepted PATCH moves a video from `init` to `uploading` | Accepted |
| [0065](0065-concurrent-patches-serialised-by-a-per-upload-advisory-lock.md) | Concurrent PATCHes are serialised by a per-upload advisory lock | Accepted |
| [0066](0066-the-gateways-object-store-credentials.md) | What the gateway's object-store credentials may do | Accepted |
| [0067](0067-chat-resume-is-a-join-with-after.md) | Chat resume is a `join` with `after`, and acks are `joined` and `sent` | Accepted |
| [0068](0068-a-failed-job-fails-its-video.md) | A job that fails for good fails its video in the same statement | Accepted |
| [0069](0069-chat-soak-flatness-per-command-delivery-and-connection.md) | Chat soak flatness judged per command, delivery and connection | Accepted |
| [0070](0070-live-chat-lossy-and-bounded.md) | A stream's live chat: joined by the stream, lossy for every viewer, bounded everywhere | Accepted |
| [0071](0071-slow-readers-are-not-reset-by-the-kernel.md) | Slow readers are not reset by the kernel: the gateway and the node channel bound their own peers | Accepted |

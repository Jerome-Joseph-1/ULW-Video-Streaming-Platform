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
| [0039](0039-openmls-behind-a-narrow-c-api.md) | OpenMLS behind a narrow C API, pinned by Cargo.lock | Accepted |

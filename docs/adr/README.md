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
| [0007](0007-admission-control-from-the-memory-budget.md) | Admission control per route, derived from the memory budget | Accepted |
| [0008](0008-expected-for-recoverable-errors.md) | std::expected for recoverable errors, exceptions for bugs | Accepted |
| [0009](0009-storage-port-at-the-durable-offset.md) | Storage is abstracted at the durable offset, not the part | Accepted |
| [0010](0010-io-uring-primary-reactor.md) | io_uring is the primary reactor, epoll the fallback | Accepted |
| [0011](0011-cloudflare-r2-object-storage.md) | Cloudflare R2 for production object storage | Accepted |

# 0009. Storage is abstracted at the durable offset, not the part

Status: Accepted
Date: 2026-09-28
Supersedes: 0004

## Context

ADR-0004 exposed multipart parts to the gateway. Part numbers, ETags and upload ids reached core
types and the database schema, and a backend without multipart upload, or with an offset-based
resumable API such as GCS resumable uploads, would have had to fake parts.
`tools/check-boundaries.sh` now fails the build on that vocabulary in core headers. The client
protocol is already offset-based: a client resumes by asking how many bytes the server holds.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Keep the part-level port (ADR-0004) | Already written; maps one to one onto S3 | Rejected: S3 vocabulary in core and in the schema; non-S3 backends fake parts |
| Offset-based port with a blocking write | The simplest possible shape | Rejected: a blocking call on the reactor thread |
| Offset-based port; parts stay in the S3 adapter; progress through an observer on the reactor thread | Core deals only in bytes and offsets, and the reactor never blocks | Accepted |

## Decision

- The public API is offset-based. `PATCH` carries `Upload-Offset`; `HEAD` returns the durable
  offset; a mismatch is answered with `409 Conflict` and the authoritative offset.
- Part numbers, ETags and upload ids stay inside `infra/storage/s3/`. Core sees an `IngestId`
  whose `backend_ref` is adapter state it persists verbatim and never reads.
- `IIngestStore::open(id, offset, observer)` takes the caller's cached durable offset. A stale-low
  value is safe: it only re-sends bytes the backend already holds, and re-uploading a part
  replaces it.
- Progress and completion arrive through `IIngestObserver`, called on the reactor thread and never
  re-entrantly from inside a session call. No port call blocks the reactor.
- `create`, `durable_offset`, `commit` and `discard` may block on the network and run off the
  reactor thread.
- The streaming rule from ADR-0004 carries over: a chunk is streamed into one in-flight backend
  request and never buffered whole. A `write` that takes 0 bytes is backpressure, and the gateway
  stops receiving until the observer fires.

## Consequences

- On S3 the durable offset advances in whole parts. A client that disconnects partway through an
  8 MiB chunk resumes from the chunk's start and re-sends up to 8 MiB.
- One port spans two threading domains. The split is a documented contract checked by tests, not
  something the type system enforces.
- The port contract is verified by a conformance suite against MinIO in CI and against R2 when
  `ULW_CONFORMANCE_LIVE` is on.
- Reopen if a backend cannot report a durable offset cheaply enough to answer every `HEAD`.

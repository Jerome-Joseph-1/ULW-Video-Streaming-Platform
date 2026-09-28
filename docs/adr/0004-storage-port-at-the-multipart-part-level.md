# 0004. Storage port at the multipart part level

Status: Superseded by 0009
Date: 2026-09-28

## Context

Uploads are resumable, up to 50 GiB, and must reach object storage from a gateway with about
600 MB for connection state (ADR-0007). S3 multipart upload maps onto that directly: a client
chunk becomes one part, and a part is durable once the store returns its ETag. Chunks are 8 MiB,
so 50 GiB is 6,400 parts, inside S3's 10,000-part ceiling.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Buffer each chunk in memory, then upload it | Retrying a failed part is trivial | Rejected: 8 MiB x 512 upload slots = 4 GiB, four times the whole box |
| Spool each chunk to local disk, then upload it | Retries from disk; memory stays flat | Rejected: doubles the I/O, needs 4 GiB of scratch at 512 slots, and delays durability until the whole chunk has arrived |
| Port exposes parts, part numbers and ETags; the gateway streams each chunk into one in-flight UploadPart | Maps one to one onto S3; resume is "list the parts" | Accepted |

## Decision

The storage port speaks in parts. The gateway starts a multipart upload, streams each chunk into
exactly one in-flight part request as the bytes arrive, records the part number and ETag in
Postgres, and completes the upload with that list.

The gateway never buffers a whole chunk. Bytes read from the client go straight into the backend
request; when the backend stops accepting, the gateway stops receiving, and the client's TCP
window closes.

## Consequences

- Memory per upload is bounded by the pump buffer, not by the chunk size.
- A part that fails midway is sent again from its start under the same part number; the
  re-upload replaces the part.
- Part numbers, ETags and upload ids appear in core types and in the database schema.
- A backend without multipart upload would have to fake parts.

Superseded by ADR-0009: exposing parts leaked S3 vocabulary into core and did not fit non-S3
backends. The streaming and backpressure reasoning above still holds and carries over.

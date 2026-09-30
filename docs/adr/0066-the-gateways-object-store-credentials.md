# 0066. What the gateway's object-store credentials may do

Status: Accepted
Date: 2026-09-29

## Context

The gateway holds a key pair for the object store (R2 in production, MinIO in dev and CI). A
token that can do everything on the bucket is easy to issue and expensive to lose: the gateway is
the process a client's bytes reach. The brief lists what the gateway needs (create, upload part,
complete, abort, list parts, head, get for presigning, put for small objects) and requires that
multipart abort be permitted; R2's abort rule is in ADR-0011. The list has not been recorded
anywhere a person issuing a token would find, and it was written before the reaper (ADR-0049)
and the playlist rewriting (ADR-0024) changed what the gateway does.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| One token for the whole platform | One secret to rotate | Rejected: the worker and the gateway need different things, and a leak of one should not be a leak of both |
| Bucket-wide read and write for the gateway | Simplest to issue | Rejected: says nothing about multipart, and grants deletion of anything |
| The operations the gateway's code makes, one token per component | The scope can be checked against the code | Accepted |

## Decision

One token per component (the worker has its own; `docs/integration/operations-contract.md`).
The gateway's token must permit exactly what `infra/storage/s3/src/s3_store.cpp` issues on its
behalf:

| Operation | Used for |
|---|---|
| CreateMultipartUpload (`POST ?uploads`) | starting an upload's session |
| UploadPart (`PUT ?partNumber&uploadId`) | each part of a PATCH |
| ListParts (`GET ?uploadId`) | the durable offset, and what to complete |
| CompleteMultipartUpload (`POST ?uploadId`) | committing an upload |
| AbortMultipartUpload (`DELETE ?uploadId`) | discarding a session, which must be permitted |
| HeadObject | the size of a finished object |
| GetObject | playlists, read and rewritten inline, and presigned GET for segments and init |
| PutObject | small objects |

The reaper (ADR-0049) runs from the gateway image with the gateway's secret
(`deploy/askedin/overlays/*/upload-reaper/cronjob.yaml`), so the same token is also used for
three more operations: ListMultipartUploads (bucket level, to sweep abandoned sessions),
ListObjectsV2, and DeleteObject (the object left at an aborted upload's key).

## Consequences

- A person issuing the token has the list; the check is that each row names a call in the
  adapter.
- Divergence from the brief, plainly:
  - The brief scopes the token "on the raw prefix". The code has no raw prefix: the source is one
    key, `videos/<video id>/raw` (`apps/gateway/src/connection.cpp`), in the same tree as the
    worker's output at `videos/<video id>/hls/`. The gateway also reads `hls/` (playlists and
    presigned segments), so a token limited to `raw` would break playback.
  - The brief's list does not include ListMultipartUploads, ListObjectsV2 or DeleteObject, which
    the reaper needs since ADR-0049.
  - Nothing in the repository enforces the scope: the adapter uses whatever token it is
    given, and the operations contract states the requirement in prose ("object read and write
    on the bucket ... must also allow multipart create ..."), less precisely than the table
    above. That document should be brought into line with this one when the tokens are issued.
- The reaper and the gateway share one token because they share a secret. Giving the reaper its
  own would let the gateway's lose the three sweep operations; not done, since it is a
  deployment change.

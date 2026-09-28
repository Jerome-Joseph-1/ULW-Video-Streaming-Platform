# 0011. Cloudflare R2 for production object storage

Status: Accepted
Date: 2026-09-28

## Context

VOD and live viewers download segments from object storage directly (ADR-0002, ADR-0014), so
egress is the cost that grows with viewers. The code already speaks S3 (SigV4, multipart) in one
adapter. The question is which S3-compatible store backs production, and what its differences
from S3 cost us.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| AWS S3 | The reference implementation; every S3 feature | Rejected: egress is billed per GB, and egress is the cost that grows with viewers |
| Backblaze B2 through its S3 API | Low storage price | Rejected: egress is free only up to a multiple of the data stored, or through partner CDNs |
| MinIO on the cluster nodes | The same software as dev and CI; no vendor | Rejected: every viewer's bytes return to the two 600 Mbit/s nodes, which ADR-0002 exists to avoid; capacity limited to the VPS disks |
| Cloudflare R2 through the S3 adapter | No egress fees for video delivery; S3 API; presigned URLs | Accepted |

## Decision

Production object storage is Cloudflare R2, reached through the single S3 adapter with an
`S3Profile::r2` profile. MinIO (`S3Profile::minio`) serves dev and CI. R2's differences live in
the profile, not in separate code paths:

- path-style addressing, region `auto`;
- every part except the last must be the same size, so an upload's chunk size is fixed when the
  upload is created;
- presigned URLs are honoured only on the `r2.cloudflarestorage.com` host, not on custom domains;
- about 1 write per second per key;
- no Object Lock, versioning or ACLs: the bucket is private and reads go through presigned URLs;
- R2 aborts incomplete multipart uploads after 7 days.

## Consequences

- An upload must complete within 7 days of creation or R2 discards its parts, so upload expiry is
  shorter than that and our own reaper cleans up first.
- Nothing may rewrite a key in a tight loop. A live packager rewrites a media playlist once per
  segment, which stays under 1 write/s as long as segments are longer than 1 s.
- Presigned URLs on the API host bypass Cloudflare's cache, and R2 bills read operations per
  request, so cost follows segment request count. Monitor the operation count.
- Without versioning, a deleted or overwritten object is gone.
- Moving to another S3-compatible store is a new profile plus a pass of the conformance suite.
- Reopen if R2 starts billing egress, or if presigned URLs work on a custom domain, which would
  put Cloudflare's cache in front of segments.

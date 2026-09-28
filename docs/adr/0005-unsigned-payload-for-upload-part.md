# 0005. UNSIGNED-PAYLOAD for UploadPart, over TLS only

Status: Accepted
Date: 2026-09-28

## Context

SigV4 signs a request together with `x-amz-content-sha256`, the SHA-256 of the body, and that
header is sent before the body. The gateway streams each 8 MiB chunk into an UploadPart as it
arrives from the client and never holds a whole chunk (ADR-0009). To send the real hash it would
have to receive and hash all 8 MiB first: 8 MiB x 512 upload slots = 4 GiB, against a 1 GB box.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Real body hash on every request | The store rejects any body that does not match the signature | Rejected for UploadPart: the hash precedes the body, so the whole part must be buffered first |
| `Content-MD5` per part | Cheaper than SHA-256; checked by the store | Rejected: also a header sent before the body; same buffering problem |
| `STREAMING-AWS4-HMAC-SHA256-PAYLOAD` (aws-chunked) | Signs each chunk as it streams; no buffering | Rejected: re-frames the body with per-chunk signatures, a second signing path to verify against every backend profile, for integrity TLS already gives |
| `UNSIGNED-PAYLOAD` on a TLS connection | Method, path, query and headers stay signed; no buffering; accepted by S3, R2 and MinIO | Accepted |

## Decision

UploadPart sends `x-amz-content-sha256: UNSIGNED-PAYLOAD`. This is safe only because the
connection to the store is TLS. The signature still covers the method, the key, the part number
and upload id in the query, and the headers, but not the body: anyone who saw those headers could
send a different body under the same signature. TLS keeps the headers unseen and the body intact
in transit.

CompleteMultipartUpload and small objects (playlists, manifests) are signed with the real SHA-256
of the body. They are small and already fully in memory.

## Consequences

- Corruption that happens outside TLS, in our process before encryption or in the store after
  decryption, is not caught by the signature.
- MinIO over plain HTTP in dev and CI gets no body integrity at all. That is acceptable for test
  data and for nothing else.
- Monitor for any production profile configured with an `http://` endpoint; it silently removes
  the integrity this decision depends on.
- Reopen if every backend profile supports trailing checksums (`x-amz-checksum-crc32c` sent after
  the body), which give integrity without buffering, or if a supported backend refuses unsigned
  payloads.

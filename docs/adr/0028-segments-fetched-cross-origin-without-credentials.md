# 0028. Viewers fetch segments cross-origin, without credentials

Status: Accepted
Date: 2026-09-29

## Context

A viewer's page is served from the app's origin, and Askedin's route sends `/api` on that
origin to the gateway, so playlist requests are same-origin and carry the `auth_token` cookie.
The segment and init URLs in those playlists are presigned URLs on the R2 API host
(ADR-0024, ADR-0011): another origin. hls.js fetches them with XHR or fetch, so the browser only
hands the bytes to the page if the bucket answers with CORS headers that allow the app origin.
Without them playback fails at the first segment, while every playlist request succeeds.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Serve segments from the app origin through the gateway | No CORS at all | Rejected: ADR-0002; every viewer's bytes through a 600 Mbit/s node |
| Send credentials to the bucket (`withCredentials`) | One login covers everything | Rejected: the bucket cannot check our cookie, it would only leak it to another host, and a credentialed CORS response may not use a wildcard origin |
| A bucket CORS rule for the app origin, requests without credentials; the signature in the URL is the authorization | Nothing new to run; what S3-compatible stores already support | Accepted |

## Decision

- Playlists stay same-origin with the page; segments are fetched cross-origin, without cookies
  or an `Authorization` header. A presigned URL signs only `host`, so it needs neither.
- The production bucket carries this CORS rule, applied by an operator (R2 dashboard, or
  `aws s3api put-bucket-cors` against the R2 endpoint); the Askedin runbook lists it with the
  bucket setup:

  ```json
  {"CORSRules": [{
    "AllowedOrigins": ["https://<app origin>"],
    "AllowedMethods": ["GET", "HEAD"],
    "AllowedHeaders": ["Range", "If-None-Match"],
    "ExposeHeaders": ["Content-Range", "Content-Length", "ETag"],
    "MaxAgeSeconds": 3600
  }]}
  ```

  One origin per environment (stage and prod each list their own). No `AllowCredentials`.
- Local MinIO answers CORS for every origin (`MINIO_API_CORS_ALLOW_ORIGIN=*` in
  `deploy/local/compose.yaml`): development pages and the E2E suite's page server listen on
  whatever loopback port is free, and the community server has no per-bucket CORS API.
- The filesystem backend has no signer. For local runs without MinIO, its objects are published
  by a development file server over `<ULW_FS_ROOT>/objects` that sends CORS headers of its own,
  such as `npx http-server --cors`, and `ULW_FS_READ_URL` names it; segment URLs are then plain,
  unsigned URLs on that server.

## Consequences

- A bucket without the rule fails in the browser only: `curl` and the integration tests fetch
  the same URLs fine. The E2E suite is what catches a missing rule, since it plays through a
  real browser from a different origin than the bucket's.
- `Range` passes preflight, so players that fetch byte ranges or resume partial segments work.
- The rule names origins, not viewers: any page on the app origin can fetch a segment whose URL
  it holds. The signature, not CORS, is the access control.
- The filesystem backend's URLs are unauthenticated; it stays a development backend.
- Reopen if segments move behind a CDN on the app's own origin, which would make them
  same-origin again.

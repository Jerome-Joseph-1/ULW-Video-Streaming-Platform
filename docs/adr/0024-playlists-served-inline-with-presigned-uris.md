# 0024. Playlists are rewritten and served inline

Status: Accepted
Date: 2026-09-28

## Context

Segment bytes come from object storage through presigned URLs (ADR-0002). HLS players resolve
relative URIs against the URL of the playlist that contains them. If the gateway answered a
playlist request with a 302 to a presigned playlist URL, the player would resolve every child
(variant playlists, segments, init segments) against the R2 URL, unsigned, and R2 would answer
403.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| 302 to a presigned playlist URL | No playlist bytes through the gateway; a trivial handler | Rejected: child URIs resolve against R2 without a signature and fail with 403 |
| Presign every URI once, when the playlist is written | No work per request | Rejected: signatures expire (R2 caps them at 7 days), and one set of URLs would be shared by every viewer |
| Rewrite playlists per request and serve them inline with 200 | Every child URI is either signed or routed back through the gateway; TTLs per viewer | Accepted |

## Decision

- The gateway reads the stored playlist, rewrites it and serves it inline with `200`.
- In the master playlist, each variant URI is rewritten to the gateway's own
  `/api/v1/videos/{id}/{rendition}/index.m3u8`, so variant playlists pass through the same
  authorization and rewriting.
- In a media playlist, every URI, `EXT-X-MAP` included, is rewritten to a presigned URL with a TTL
  of `max(2 x duration, 1 h)`, capped by the profile's limit (R2: 604,800 s, 7 days). Twice the
  duration leaves room for pauses and seeks; the 1 h floor covers short videos.
- Responses carry `Cache-Control: private, max-age=60`: they hold per-viewer signatures, so no
  shared cache may keep them, and the viewer's own cache reuses one for at most a minute.
- Relative URIs resolve against the playlist's directory. A URI containing `..` or starting with
  `/` is rejected, so a playlist cannot point the signer at a key outside its own video.

## Consequences

- Every playlist fetch costs a small object read and a rewrite on the gateway. Signing is pure
  computation per URI: a two-hour video with 6 s segments has 7,200 / 6 = 1,200 URIs in each
  media playlist.
- A viewer paused past the TTL gets 403 on the next segment, and the player must reload the
  playlist to continue.
- Live media playlists need a shorter cache lifetime than 60 s (ADR-0014).
- Monitor rejected playlist URIs; one in a stored playlist means the worker wrote something
  wrong.
- Reopen if presigned URLs become usable on a cacheable domain (ADR-0011).

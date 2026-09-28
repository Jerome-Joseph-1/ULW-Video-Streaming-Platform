# 0002. VOD playback never proxies segment bytes

Status: Accepted
Date: 2026-09-28

## Context

A viewer pulls its rendition's bitrate for as long as it watches. The gateway runs on a node with
at most 1 GB of RAM and a 600 Mbit/s port that it shares with uploads and realtime media. At an
illustrative 3 Mbit/s per viewer, 600 Mbit/s carries 600 / 3 = 200 concurrent viewers with nothing
left for anything else. Object storage (ADR-0011) can serve the same bytes directly, and its
capacity does not depend on our nodes.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Proxy segments through the gateway | One URL space; an access check on every segment; no signed URLs in circulation | Rejected: every viewer's bandwidth passes through a 1 GB, 600 Mbit/s box; about 200 viewers at 3 Mbit/s fill the port |
| Public bucket paths | Nothing to sign; cacheable by anyone | Rejected: no authorization; a private video is readable by anyone who has the URL, forever |
| Signed cookies validated by an edge in front of the bucket | One grant covers every segment; playlists need no rewriting | Rejected: needs an edge component that validates our signatures, which the bucket cannot do on its own; one more thing to run and secure |
| The gateway authorizes and issues presigned URLs | Segment bytes go straight from storage to the viewer; the gateway only handles playlists of a few KB | Accepted |

## Decision

The gateway authenticates the viewer, checks access to the video, and answers with playlists
whose segment URIs are presigned URLs on the object store (ADR-0024 fixes the mechanics). Object
storage, or a CDN in front of it, serves every segment byte. Segment bytes never transit the
gateway.

This covers VOD and live HLS (ADR-0014). Realtime media is a different tier with different rules
(ADR-0012).

## Consequences

- A presigned URL is a bearer token until it expires: anyone the viewer passes it to can fetch
  that segment. Revoking access does not invalidate URLs already issued; the TTL bounds the
  exposure.
- Authorization happens once per playlist fetch, not per segment.
- Playlists carry per-viewer signatures, so they are rewritten on every request and cannot sit in
  a shared cache.
- An object storage outage is a playback outage; the gateway has nothing to fall back on.
- Monitor gateway egress per playback request. It should be playlist-sized; growth means
  something has started proxying.
- Reopen if an edge in front of the bucket can enforce per-viewer authorization without our
  servers in the byte path.

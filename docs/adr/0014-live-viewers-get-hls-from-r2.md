# 0014. Live-stream viewers get HLS from R2, not WebRTC fan-out

Status: Accepted
Date: 2026-09-28

## Context

A live stream is published with WHIP (WebRTC) or RTMP and packaged to HLS. Viewers could join
through the SFU like call participants, or fetch HLS like VOD viewers. A node has 540 Mbit/s for
media (ADR-0012), shared with calls.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| WebRTC fan-out through the SFU | Sub-second latency; the same stack as calls | Rejected: every viewer is a downstream leg through the 600 Mbit/s node; at 2 Mbit/s per viewer, 540 / 2 = 270 viewers fill it, with no room left for calls |
| LL-HLS (partial segments, blocking playlist reloads) | Latency of a few seconds while keeping HLS scaling | Rejected for now: a blocking reload needs a server that holds the request open until the next part exists, which a presigned R2 URL cannot do |
| HLS in R2, served exactly like VOD | Viewer bandwidth scales on R2, not on our port; presigned URLs and playlist rewriting already exist | Accepted |

## Decision

Live-stream viewers get HLS from R2 exactly as VOD viewers do. The packager writes segments and
playlists to R2; the gateway authorizes the viewer and rewrites playlists with presigned URLs
(ADR-0002, ADR-0024). The SFU carries the publisher's ingest and never fans out to viewers.
LL-HLS is out of scope.

## Consequences

- Glass-to-glass latency is several segments. Players start at least three target durations
  back from the live edge (RFC 8216, section 6.3.3), so with target duration T the delay is at
  least 3T plus encoding, packaging and upload time. Live chat has to tolerate that lag.
- A live media playlist changes every segment and is re-fetched every target duration. Its cache
  lifetime must be shorter than T, not the 60 s that ADR-0024 sets for VOD playlists.
- Playlist reloads land on the gateway: viewers / T requests per second, for example
  1,000 viewers at T = 4 s = 250 requests/s. Segment bytes still go to R2.
- The packager rewrites each media playlist once per segment, inside R2's 1 write/s per key as
  long as T is over 1 s (ADR-0011).
- Reopen if viewers need interactive latency (co-hosting, auctions); that needs LL-HLS from a
  server that can hold requests, or a distribution tier with its own bandwidth.

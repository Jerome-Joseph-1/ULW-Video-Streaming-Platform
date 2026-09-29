# Live streams

> **Draft**, except [Watching a stream](#watching-a-stream), which is final. Publishing and
> the recording may still change until milestone M33 merges.

A broadcaster publishes into the realtime tier (WHIP first, RTMP as a fallback). A packager
turns the stream into HLS and writes its segments to the same object store VOD uses. Viewers
never receive WebRTC: they play live HLS from the store (ADR-0014), fetched the same way as VOD
([videos-and-playback.md](videos-and-playback.md)): playlists through the gateway, segments from
presigned store URLs. Live playlists are cached for less than VOD's 60 s. When a stream ends its
recording becomes an ordinary video that goes through `processing` to `ready`. Live chat is a
chat room in lossy delivery mode ([chat.md](chat.md)).

## Watching a stream

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/connection.cpp (start_live, on_live_playlist), apps/gateway/src/live_manifest_cache.cpp, apps/gateway/src/playback.cpp (build_live_playlist), docs/adr/0059-live-playlists-through-a-single-flight-cache.md -->

A live stream is one media playlist (no master), fetched through the gateway like a VOD
playlist and reloaded by the player as it grows. Its segments come from the store on signed
URLs, exactly as VOD segments do: the [CORS rule](videos-and-playback.md#cors) and the
[player setup](videos-and-playback.md#playing) of VOD apply unchanged, with the live URL as the
source.

| Endpoint | Success | Errors |
|---|---|---|
| `GET /api/v1/live/{id}/index.m3u8` | `200`, `application/vnd.apple.mpegurl`, the stream's live playlist | `401`, `404`, `503`, `500` |

- **Who may watch.** Any signed-in user (a valid token, as for every API route) may watch any
  stream by its id. A live stream is a broadcast; its recording is the broadcaster's own video
  like any upload. `{id}` is the stream id the packager was started with: 1 to 64 of
  `A-Z a-z 0-9 _ -`.
- **The playlist.** RFC 8216 live: `EXT-X-VERSION:7`, `EXT-X-TARGETDURATION` (the segment length,
  2 to 10 s, 2 by default), `EXT-X-MEDIA-SEQUENCE`, which never decreases, the last 10 segments
  (fMP4, each with `EXT-X-PROGRAM-DATE-TIME`), and after a packager restart
  `EXT-X-DISCONTINUITY` with a new `EXT-X-MAP`. Every URI, `EXT-X-MAP`'s included, is a URL
  signed for one hour.
- **The end.** When the stream ends the playlist ends with `EXT-X-ENDLIST`: a player plays out
  what is listed and fires its `ended` event. It stays fetchable, with the same window, until the
  bucket's lifecycle rule expires the stream's objects.
- **Freshness.** The gateway keeps each stream's rewritten playlist for half a target duration
  and reads the store once per interval however many viewers ask, so a playlist is at most half
  a segment behind the packager's. Live playlists are answered with
  `Cache-Control: private, no-cache`; an ended one with `private, max-age=60`.
- **Latency.** A player starts three target durations behind the newest segment (RFC 8216
  6.3.3). Measured end to end in hls.js at T = 2 s (ADR-0059): 7.5 to 7.7 s from the moment a
  frame is made at the publisher to the moment it is on screen, for a viewer who joins a
  running stream. Plan on about 4T for a different segment length.

| Status | Meaning | Client action |
|---|---|---|
| `401` | No token, or a bad one | Sign in again |
| `404` | No stream by that id has published a segment yet, or it is not a valid stream id | A stream about to start: retry after a second or two (hls.js retries a manifest load by itself; give it `manifestLoadPolicy` retries enough for the wait you want). Otherwise stop |
| `503` | The store is throttling or unreachable | Retry with backoff; hls.js does |
| `500` | The stored playlist is broken or a URL could not be signed | Report with `X-Request-Id` |

```http
GET /api/v1/live/launch-2026/index.m3u8
Cookie: auth_token=<jwt>

HTTP/1.1 200 OK
Content-Type: application/vnd.apple.mpegurl
Cache-Control: private, no-cache

#EXTM3U
#EXT-X-VERSION:7
#EXT-X-TARGETDURATION:2
#EXT-X-MEDIA-SEQUENCE:118
#EXT-X-INDEPENDENT-SEGMENTS
#EXT-X-MAP:URI="https://<bucket host>/live/launch-2026/init_0.mp4?X-Amz-Algorithm=AWS4-HMAC-SHA256&...&X-Amz-Expires=3600&..."
#EXT-X-PROGRAM-DATE-TIME:2026-09-29T17:10:36.000Z
#EXTINF:2.000000,
https://<bucket host>/live/launch-2026/seg_0_118.m4s?X-Amz-Algorithm=AWS4-HMAC-SHA256&...
...
```


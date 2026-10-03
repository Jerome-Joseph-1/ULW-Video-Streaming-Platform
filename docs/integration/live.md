# Live streams

> **Draft.** [Starting a stream](#starting-a-stream) is served by the gateway (ADR-0091),
> [Publishing](#publishing) is settled by M30, [Watching a stream](#watching-a-stream) by M31 and
> [When a stream ends](#when-a-stream-ends) by M33.

A broadcaster publishes into the realtime tier with WHIP (RFC 9725). The SFU's recorder relays
the stream to a packager, which turns it into HLS and writes its segments to the same object
store VOD uses (ADR-0046, ADR-0047). Viewers never receive WebRTC: they play live HLS from the
store (ADR-0014), fetched the same way as VOD ([videos-and-playback.md](videos-and-playback.md)):
playlists through the gateway, segments from presigned store URLs. Live playlists are cached for
less than VOD's 60 s. When a stream ends its recording becomes an ordinary video that goes
through `processing` to `ready`. Live chat is a chat room in lossy delivery mode
([chat.md](chat.md)), joined by the stream's id, and open from the moment the stream is started.

## Starting a stream

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/connection.cpp (start_stream_route, respond_stream, fail_live), apps/gateway/src/live_streams.cpp, infra/postgres/src/live_streams.cpp, migrations/0011_live_streams.sql, docs/adr/0091-the-stream-service-lives-in-the-gateway.md -->

The gateway's stream service starts a stream for the signed-in user, hands its owner publisher
tickets, takes it live, and ends it. Every request is authenticated like the rest of the API
(bearer token or the auth cookie, and with the cookie an allowed `Origin`), carries no body, and
counts against the user's request rate. The owner of a stream is the user whose token started
it; only they can get its tickets, take it live or end it, and someone else's stream answers
`404`, as a video does. A user has **one unfinished stream at a time**.

| Endpoint | Who | Success | Errors |
|---|---|---|---|
| `POST /api/v1/live` | Any signed-in user | `201` a new stream, with its first ticket in `publish`; `200` the user's unfinished stream, with a fresh ticket | `401`, `403`, `429`, `503`, `500` |
| `POST /api/v1/live/{id}/ticket` | The owner | `200` a fresh publisher ticket: `url`, `token`, `expires_at` | `401`, `404`, `409`, `429`, `503`, `500` |
| `POST /api/v1/live/{id}/start` | The owner, after the WHIP POST's `201` | `200` the stream, `live` | `401`, `404`, `409`, `429`, `503`, `500` |
| `POST /api/v1/live/{id}/end` | The owner | `200` the stream, `ended` | `401`, `404`, `429`, `503`, `500` |
| `GET /api/v1/live/{id}` | Any signed-in user | `200` the stream | `401`, `404`, `429`, `503`, `500` |

The stream, as every one of these answers it (`Cache-Control: no-store`):

| Field | Value |
|---|---|
| `id` | The stream id: a UUID. It is also the stream's name for its playlist, its chat and its recording |
| `state` | `starting` (tickets issued, nothing relayed yet), `live` (its packager runs and the publisher is relayed to it) or `ended`. It only moves forward |
| `playlist` | Where viewers watch: `/api/v1/live/{id}/index.m3u8` ([Watching a stream](#watching-a-stream)) |
| `created_at`, `live_at`, `ended_at` | Unix seconds, or `null` until then |
| `ended_by` | `null` until it ends; then `owner` (ended with `end`), `finished` (the publisher went: a WHIP DELETE, the media server dropping it, 12 hours, or a broken stream), `failed` (its packager could not run) or `timeout` (not taken live within 10 minutes of `POST /api/v1/live`, or past 13 hours) |
| `video_id` | The owner only: the recording's video once it is queued, else `null` ([When a stream ends](#when-a-stream-ends)). Absent for anyone else |
| `publish` | `POST /api/v1/live` only: the first publisher ticket |

The flow:

1. `POST /api/v1/live`. Keep `id`; `publish` is the ticket for the WHIP POST. Asking again
   before the stream ends answers the same stream with a fresh ticket (`200`), so a lost answer
   is retried safely. To start another stream, end this one first.
2. `POST` the offer to `publish.url` with `publish.token` ([Publishing](#publishing)).
3. Once the POST is answered `201`, `POST /api/v1/live/{id}/start`. The gateway starts the
   stream's packager, waits for it (seconds; up to 30), and has the media server relay the
   publisher to it; the answer is the stream, `live`. Call it within 30 s of the POST's `201`:
   the media server's recorder looks for the publisher that long. It is idempotent: retry it on
   `503` or a lost answer. A stream nobody takes live within 10 minutes is ended (`timeout`).
4. Before every WHIP request after the POST (each PATCH, an ICE restart, the DELETE), `POST
   /api/v1/live/{id}/ticket` for a fresh ticket.
5. To stop, DELETE the WHIP session with a fresh ticket, or `POST /api/v1/live/{id}/end`, or
   both. Either ends the stream, and its playlist gets `EXT-X-ENDLIST` within a second or two.
   `end` answers the ended stream, and is idempotent.
6. Poll `GET /api/v1/live/{id}` for `video_id`; then follow the video as any other
   (`GET /api/v1/videos/{video_id}`).

A viewer's client needs only the id: `GET /api/v1/live/{id}` says whether it is live and where
the playlist is, and the stream's chat is joined by the same id ([chat.md](chat.md)).

| Status | Meaning | Client action |
|---|---|---|
| `401` | No token, or a bad one | Sign in again |
| `403` | The auth cookie from a page that is not allowed, or without `Origin` | Send the request from the app's own page, or with `Authorization` |
| `404` | No such stream, or not the caller's to act on | Stop |
| `409` | The stream has ended: no ticket, and no going live again | Start a new stream |
| `429` | The user's request rate is used up | Wait `Retry-After` |
| `503` | The platform runs as many streams as it takes (`Retry-After: 60`), or the database, the media server or the packager runtime is unavailable or slow (`Retry-After: 2`). An `end` answered `503` has ended the stream all the same; repeat it so the publisher is disconnected | Retry after `Retry-After` |
| `500` | A dependency refused the request as made | Report with `X-Request-Id` |

```http
POST /api/v1/live
Authorization: Bearer <jwt>
Content-Length: 0

HTTP/1.1 201 Created
Content-Type: application/json
Cache-Control: no-store

{"id":"01999a3c-7b2e-7c41-9d0e-3a5f4c2b1e77","state":"starting",
 "playlist":"/api/v1/live/01999a3c-7b2e-7c41-9d0e-3a5f4c2b1e77/index.m3u8",
 "created_at":1759510800,"live_at":null,"ended_at":null,"ended_by":null,"video_id":null,
 "publish":{"url":"https://<media host>/whip/v1","token":"<jwt>","expires_at":1759510860}}
```

## Publishing

The stream's owner gets a **publisher ticket** from the stream service: the first with the
stream, and a fresh one from `POST /api/v1/live/{id}/ticket` ([Starting a
stream](#starting-a-stream)). The ticket has three fields:

| Field | Meaning |
|---|---|
| `url` | The WHIP endpoint, `https://<media host>/whip/v1`. |
| `token` | The bearer token for one WHIP request. |
| `expires_at` | Unix seconds, 60 s after issue. |

The client then speaks WHIP to `url` (ADR-0053), and asks the stream service for a **fresh
ticket before every request after the POST**: each PATCH, an ICE restart, the DELETE. A ticket
is only good for a minute, and the media server checks the token on every request of the
session, so the POST's ticket is refused for a DELETE an hour later. Each fresh ticket names the
same publisher, and the stream service issues one only while the stream is the owner's current
one, so a stream that was ended or moved to a new session cannot be touched with a ticket it
handed out earlier. Nothing else of the platform is between the client and the media server.

| Request | Body | Success | Response |
|---|---|---|---|
| `POST <url>` | `Authorization: Bearer <token>`, `Content-Type: application/sdp`, the offer | `201` | The answer (`application/sdp`); `Location`, the session's resource, relative to `url`; `ETag`, the ICE session; `Link` headers with `rel="ice-server"` (TURN URLs with `username` and `credential`) |
| `PATCH <resource>` | A fresh ticket's token; `Content-Type: application/trickle-ice-sdpfrag`, `If-Match: <ETag>`; an SDP fragment with `a=ice-ufrag`, `a=ice-pwd`, one `m=` line, the `a=mid` of the first media section and `a=candidate` lines | `204` | Nothing; the candidates are added to the session |
| `PATCH <resource>` | As above with `If-Match: *`: an ICE restart | `200` | The server's fragment for the new ICE session, and its `ETag` |
| `DELETE <resource>` | A fresh ticket's token | `200` | The session ends, and with it the stream |

- **Media.** Send only: one audio track (Opus) and one video track (VP8 or H.264), bundled. The
  stream is re-encoded to one 720p rendition at 30 fps with a keyframe every segment, whatever
  the source's own keyframe interval.
- **ICE servers.** On Askedin's cluster media reaches the media server only through TURN, and the
  TURN servers come in the `201`'s `Link` headers, not from `OPTIONS`. A browser therefore
  trickles: create the offer, POST it before calling `setLocalDescription`, call
  `setConfiguration` with the `Link` servers, then `setLocalDescription(offer)` and
  `setRemoteDescription(answer)`, and PATCH the candidates as they come (RFC 9725 sections 4.3.1
  and 4.6). An encoder that puts all its candidates in the offer (GStreamer's `whipsink`) works
  only where it reaches the media server without TURN.
- **One source per stream.** Every ticket for a stream names the same publisher. A second POST
  replaces the running session, which the server disconnects; it never adds a second source.
  It is not a clean continuation either: viewers may see the new session after a jump, or the
  stream may end. To restart, DELETE first and publish as a new stream. A DELETE for the
  replaced session answers `200` and leaves the new one running.
- **Ending.** A DELETE ends the stream at once: its playlist gets `EXT-X-ENDLIST` within a
  second. A source that disappears without one is dropped once the media server's ICE checks
  give up on it (17 s in three local runs of a killed `whipsink`), and the stream ends then.
- **Encoders with one fixed token.** An encoder configured with a token rather than a way to ask
  for one (OBS, `whipsink`) must POST within the ticket's minute, and its own DELETE after that
  is refused (`401`); its stream then ends through the drop above, not at once. The page that set
  the encoder up calls `start` once the encoder shows it is connected, and `end` to stop it at
  once.

| Status | Meaning |
|---|---|
| `400` | The offer or fragment does not parse, the fragment's `mid` is not the first media section's, or its `a=ice-ufrag`/`a=ice-pwd` are not the session's. Do not retry unchanged |
| `401` | The token is missing, not a publisher ticket for this stream, or expired. Ask for a fresh ticket |
| `404` | PATCH on a session that no longer exists (replaced, dropped or deleted) |
| `413` | The offer or fragment is over 1 MiB |
| `428` | PATCH without `If-Match` |
| `500` | PATCH whose `If-Match` names another ICE session than the current one; also the media server failing. Start a new session rather than retrying the PATCH |
| `503` | The media server is unavailable. Retry the POST with a fresh ticket |

A POST is not refused for coming late: the media server allows a minute of clock skew past a
ticket's `expires_at`, and within it re-creates a stream's room that stood empty long enough to
be dropped, and answers `201`. After that the ticket is `401`; ask for a new one.

RTMP is not offered (ADR-0053): browsers cannot send it, and OBS 30 or later, GStreamer and
FFmpeg 8.0 or later publish with WHIP.

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
  like any upload ([When a stream ends](#when-a-stream-ends)). `{id}` is the stream's `id`
  ([Starting a stream](#starting-a-stream)); a stream an operator started by hand has the id it
  was given, 1 to 64 of `A-Z a-z 0-9 _ -`.
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

## When a stream ends

<!-- apps/live-packager/src/recorder.cpp, infra/postgres/src/live_recordings.cpp, docs/adr/0055-a-live-recording-is-remuxed-from-the-stored-segments.md -->

A stream ends when its broadcaster disconnects, when it is ended explicitly, when it reaches
12 hours, or when it breaks; its live playlist then ends with `EXT-X-ENDLIST` and players stop
cleanly. A stream that ended with at least one segment becomes exactly one video, however many
times its end is observed:

| Field | Value |
|---|---|
| `owner` | The broadcaster: the Askedin user id the stream was started for. Only they can see or play it, as with an upload |
| `title` | `Live stream <stream id>` |
| `state` | `processing` as soon as the recording is stored, then `ready` (or `failed`) exactly as an upload's video ([videos-and-playback.md](videos-and-playback.md#lifecycle)) |
| `duration_ms` | The whole stream. A stream whose packager restarted is one video: its parts are joined, without the gap between them; a part without audio is silent in it |

The video exists, in `processing`, once the stream has been read back from the store and
copied into one file (far faster than real time); from there it is polled like any video,
`GET /api/v1/videos/{id}`. Its renditions are the VOD ladder for the stream's resolution. A
stream that ends with no media becomes no video.

The recording is transcoded like an upload, from scratch space three times its size. On the
production worker (30 GiB) that is 10 GiB of recording: about 70 minutes at 20 Mbit/s, 4.8 hours
at 5 Mbit/s. A longer recording's video goes to `failed` with `error_reason` `no scratch space for
the source`. A stream whose stored media cannot be read back (its segments expired, or ffmpeg
refuses them) becomes no video either; the platform records why (`live_recordings.failure`).

`GET /api/v1/live/{id}` answers the owner the stream's video as `video_id` once the recording is
queued ([Starting a stream](#starting-a-stream)); until then, and for a stream that becomes no
video, it is `null`. The platform keeps the pair (`live_recordings`: stream id to video id or to
the reason there is none, written in the same transaction as the video), and the packager logs
`recording: queued as video <id>`.

# Live streams

> **Draft.** Publishing (below) is settled by M30 and [When a stream ends](#when-a-stream-ends)
> by M33. How a client asks for its publisher ticket and how viewers fetch the live playlist are
> not served yet and may still change.

A broadcaster publishes into the realtime tier with WHIP (RFC 9725). The SFU's recorder relays
the stream to a packager, which turns it into HLS and writes its segments to the same object
store VOD uses (ADR-0046, ADR-0047). Viewers never receive WebRTC: they play live HLS from the
store (ADR-0014), fetched the same way as VOD ([videos-and-playback.md](videos-and-playback.md)):
playlists through the gateway, segments from presigned store URLs. Live playlists are cached for
less than VOD's 60 s. When a stream ends its recording becomes an ordinary video that goes
through `processing` to `ready`. Live chat is a chat room in lossy delivery mode
([chat.md](chat.md)).

## Publishing

The stream's owner gets a **publisher ticket** from the stream service, on the same
authenticated path as a call ticket ([calls.md](calls.md)); that request is not fixed yet. The
ticket has three fields:

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
  is refused (`401`); its stream then ends through the drop above, not at once. A server-side
  DELETE on its behalf waits for the stream service (ADR-0053).

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

## When a stream ends

<!-- apps/live-packager/src/recorder.cpp, infra/postgres/src/live_recordings.cpp, docs/adr/0054-a-live-recording-is-remuxed-from-the-stored-segments.md -->

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

No endpoint maps a stream to its video yet. The platform keeps the pair (`live_recordings`:
stream id to video id or to the reason there is none, written in the same transaction as the
video), and the packager logs
`recording: queued as video <id>`, for the component that owns stream lifecycles to report to
clients.

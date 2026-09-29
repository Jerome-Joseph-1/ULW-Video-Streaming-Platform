# Live streams

> **Draft: changes until milestone M33 merges.** Publishing (below) is settled by M30. How a
> client asks for its publisher ticket, how viewers fetch the live playlist, and the recording
> are not served yet and may still change.

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
| `token` | The bearer token for every WHIP request of this publish: the POST, each PATCH, the DELETE. |
| `expires_at` | Unix seconds: 12 h and 60 s after issue, the longest stream plus the time to connect. |

The POST must follow within 60 s of the ticket: the media server drops a stream's room that
nobody has joined for that long, and the POST is then refused (below). Ask for a new ticket
rather than retrying an old one.

The encoder or browser then speaks WHIP to `url` with `Authorization: Bearer <token>`
(ADR-0056). Nothing else of the platform is in the way.

| Request | Body | Success | Response |
|---|---|---|---|
| `POST <url>` | `Content-Type: application/sdp`, the offer | `201` | The answer (`application/sdp`); `Location`, the session's resource, relative to `url`; `ETag`, the ICE session; `Link` headers with `rel="ice-server"` (TURN URLs with `username` and `credential`) |
| `PATCH <resource>` | `Content-Type: application/trickle-ice-sdpfrag`, `If-Match: <ETag>`; an SDP fragment with `a=ice-ufrag`, `a=ice-pwd`, one `m=` line, the `a=mid` of the first media section and `a=candidate` lines | `204` | Nothing; the candidates are added to the session |
| `PATCH <resource>` | As above with `If-Match: *`: an ICE restart | `200` | The server's fragment for the new ICE session, and its `ETag` |
| `DELETE <resource>` | None | `200` | The session ends, and with it the stream |

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
  stream may end. To restart, DELETE first and publish as a new stream.
- **Ending.** A DELETE ends the stream at once: its playlist gets `EXT-X-ENDLIST` within a
  second. A source that disappears without one is dropped after the media server's ICE timeout
  (20 s or more), and the stream ends then.

| Status | Meaning |
|---|---|
| `400` | The offer or fragment does not parse, the fragment's `mid` is not the first media section's, or its ICE credentials are not the session's. Do not retry unchanged |
| `401` | The token is missing, not a publisher ticket for this stream, or expired. Ask for a new ticket |
| `404` | PATCH or DELETE on a session that no longer exists (replaced, dropped or deleted) |
| `413` | The offer or fragment is over 1 MiB |
| `428` | PATCH without `If-Match` |
| `5xx` | The stream's room is gone (the POST came more than 60 s after the ticket), or the media server is unavailable. Ask for a new ticket |

RTMP is not offered (ADR-0056): browsers cannot send it, and OBS 30 or later, GStreamer and
FFmpeg 8.0 or later publish with WHIP.

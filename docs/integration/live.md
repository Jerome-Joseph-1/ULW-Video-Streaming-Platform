# Live streams

> **Draft**, except [When a stream ends](#when-a-stream-ends), which is final. Viewers cannot
> yet play live HLS through the gateway; the packager and the recording are on `main`.

A broadcaster publishes into the realtime tier (WHIP first, RTMP as a fallback). A packager
turns the stream into HLS and writes its segments to the same object store VOD uses. Viewers
never receive WebRTC: they play live HLS from the store (ADR-0014), fetched the same way as VOD
([videos-and-playback.md](videos-and-playback.md)): playlists through the gateway, segments from
presigned store URLs. Live playlists are cached for less than VOD's 60 s. When a stream ends its
recording becomes an ordinary video that goes through `processing` to `ready`. Live chat is a
chat room in lossy delivery mode ([chat.md](chat.md)).

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

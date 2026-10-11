# 0105. A live stream's visibility is set by the operator's backend or its owner and enforced here, before the playlist cache, from a short-lived answer per stream and per room member

Status: Accepted
Date: 2026-10-11

## Context

ADR-0059 opened every live playlist to any signed-in viewer who has the stream's id: when it was
written, nothing recorded a stream's owner or audience. The stream service (ADR-0092) has since
given every stream a row with its owner, and ADR-0097 gave videos a visibility. ADR-0097 left
streams out and named the result:

> Until then the live tail is exposed: anyone signed in who learns a stream's id can watch it
> while it is live, and so see what its recording will show before the broadcaster shares (or
> declines to share) the recording. A follow-up gives a stream a visibility of its own, checked
> on `GET /api/v1/live/{id}/index.m3u8` as here.

ADR-0059 said where the check belongs: where `start_live` resolves the id, before the playlist
cache is asked, so the cache stays one copy per stream.

What had to hold:

- **Nothing changes for today's clients.** Live streams are Stable (versioning.md): a stream
  nobody sets a visibility on must be watched exactly as before.
- **The playlist is reloaded constantly.** A player reloads it about once per target duration
  (2 s by default). ADR-0059's cache exists so that a thousand viewers cost the store one read
  per stream per half interval. ADR-0097 reads the catalog on every VOD request, but a VOD
  playlist is fetched once per playback, not every two seconds by every viewer.
- **One rule with videos.** The same three forms, `private`, `unlisted` and `room:<id>`, the same
  member rule (a room shares only while it lists the owner), and the same `404` for someone
  refused as for a stream that does not exist.
- **Streams started outside the service** (an operator's packager run by hand, the e2e
  sandbox's) have no row and no owner.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Read the row and the viewer's membership on every playlist request | Exact: a change applies at the next request on every gateway | Two point reads per viewer per reload: at 1,000 viewers and T = 2 s, about 1,000 reads a second against the stream service's pool of 2 sessions, for an answer that rarely changes | Rejected |
| Cache each stream's row for a few seconds, shared by its viewers, and each room member's membership likewise, both single-flight | An unlisted stream costs one read per interval whatever its audience; a room-shared one one per member per interval; a change takes at most the interval on other gateways | A revocation is not instant on other gateway processes | Accepted, 5 s |
| Invalidate across gateways with LISTEN/NOTIFY on `live_streams` and `chat_members` | Near-instant everywhere | A LISTEN session per gateway, and the notification plumbing ADR-0097 rejected for the same reason | Rejected: 5 s is shorter than a viewer's player buffer, and segment URLs handed out stay valid for an hour anyway |
| Make new streams private by default | Safer default | Every existing client that starts a stream and shares its id would stop working: a breaking change to a Stable surface | Rejected |
| Grants per user for streams, as for videos | Parity with videos | No caller has asked; the service API would need a stream half | Not done here |
| A visibility given at `POST /api/v1/live` | One request | The create takes no body today; a `PATCH` before `start` does the same and keeps the create unchanged | Rejected |
| The owner alone sets it | Self-serve | The product's backend could not apply its own policy to a stream, as ADR-0100 lets it for a video | Rejected: the backend sets it, and the owner may too, as for videos |
| Check the stream's live chat by the same rule | The chat is the stream's | Chat joins a stream's room by its name (ADR-0070) on another service with its own store; a check there is its own change | Not done here |

## Decision

- **Model.** A stream carries a `core::Visibility`, `unlisted` unless its owner sets another
  (`core::ports::LiveStream::visibility`). One rule, `core::ports::may_watch(stream, viewer,
  room_member)`: the owner always; anyone while unlisted; while shared with a room, those
  `chat_members` lists in it together with the owner; nobody else while private.
- **Schema** (migration 0020). `live_streams.visibility` (`text`, default `'unlisted'`) and
  `visibility_room` (`uuid`), with the checks 0016 put on `videos`, added and validated at once:
  the table holds one row per stream ever started, a few an hour.
- **Who sets it.** Who may watch is the product's policy and the platform only enforces it, as
  ADR-0100 has it for videos: the operator's backend sets it for any stream with
  `PATCH /api/v1/service/live/{id}` (the service token, in the `Authorization` header only), and
  the stream's owner for their own with `PATCH /api/v1/live/{id}`, both with the body
  `PATCH /api/v1/videos/{id}` takes, in any state. Not the owner's stream: `404`. A room that
  does not list the stream's owner: `403` `not_member`, whoever asks, decided in the same
  statement as the update. The answer is the stream object as its owner sees it, which now
  carries `visibility` (for its owner and the backend only).
- **Who may watch.** `GET /api/v1/live/{id}/index.m3u8` asks `LiveStreams::known_watch` before
  the playlist cache. It answers from what the process read in the last `watch_fresh` (5 s): the
  stream's row, and for a room-shared stream the viewer's membership. Only when that is not known
  does `watch` read it, one read per stream and one per (stream, viewer) in flight at a time,
  shared by every request that needs it. A refused viewer gets the empty `404`. A stream with no
  row, or an id the service never gives (not a UUID), is a broadcast as ADR-0059 had it.
  `GET /api/v1/live/{id}` reads the row afresh, as it did, and applies the same rule.
- **Bounds.** At most 1,024 streams and 65,536 (stream, viewer) memberships remembered per
  process, expired ones dropped first. Nothing is stored for a viewer of an unlisted or private
  stream.
- **Freshness.** A change made through a process applies there at once (it drops what it
  remembered of the stream); others see it within 5 s. A member taken off the room likewise.

## Consequences

- Nothing changes for a stream nobody sets a visibility on: unlisted is what every stream was.
- A broadcaster can keep a stream to one room, or to themselves, from its first segment, by
  setting it before `start`.
- Viewers of an unlisted stream cost one row read per 5 s per gateway process between them; a
  room-shared stream adds one membership read per member per 5 s. The stream service's two
  database sessions carry that at the platform's caps (two streams at once by default).
- A revocation takes up to 5 s on other processes, and segment URLs a playlist handed out stay
  valid for their hour, as ADR-0097 accepted for videos.
- **Not covered: the stream's live chat.** It is joined by the stream's name (ADR-0070), so a
  private or room-shared stream's chat is still open to anyone signed in who has the id. A
  follow-up applies the stream's visibility to joins of its chat room.
- Streams started outside the service stay open to every signed-in viewer.

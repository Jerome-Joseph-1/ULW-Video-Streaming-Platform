# Chat

> **Draft until phase 2 is tagged.** Messages, acks, resume, history, member lists and
> [presence](#presence) below are what `main` does. Nothing here is expected to change before
> the tag, but it is not a compatibility promise until then.

Chat is its own service, `chat_server`, separate from the video gateway (ADR-0019). Clients hold
one WebSocket to it and send JSON messages in text frames.

## Connecting

<!-- apps/chat/src/session.cpp (route, answer_request), apps/chat/src/config.cpp, docs/adr/0036-chat-server-client-edge.md -->

| | |
|---|---|
| Endpoint | `GET /rt` with a WebSocket upgrade (RFC 6455, version 13) on the chat host: `wss://<CHAT_HOST>/rt` |
| Subprotocols, extensions | None. `permessage-deflate` is not offered. |
| Auth | The Askedin token, as for the gateway ([auth.md](auth.md)): `Authorization: Bearer <token>`, or the `ULW_AUTH_COOKIE` cookie |
| Cookie and `Origin` | A cookie token is accepted only when the request's `Origin` is listed exactly in `ULW_ALLOWED_ORIGINS` (`scheme://host[:port]`, comma separated). With no list configured, cookies are refused. A bearer token is accepted from any origin. |

Upgrade refusals (the connection is closed after the response, and the body is empty):

| Status | When |
|---|---|
| `400` | Malformed upgrade or `Sec-WebSocket-Key`, or the request carries a body |
| `426` with `Upgrade: websocket` | `GET /rt` that is not an upgrade, or a WebSocket version other than 13 |
| `401` | No usable token, or the token fails verification |
| `403` | Cookie token without an allowed `Origin` |
| `404` | Any path other than `/rt`, `/healthz`, `/readyz`, `/metrics`, or any method other than `GET` |
| `503` | The key set cannot be fetched; retry |

A browser cannot set `Authorization` on a WebSocket, so a browser client uses the cookie from a
listed origin. Native apps send the bearer header.

## Messages

<!-- apps/chat/src/envelope.hpp, apps/chat/src/envelope.cpp, apps/chat/src/chat_service.cpp, docs/adr/0043-chat-service-policy-between-edge-and-rooms.md, docs/adr/0054-messages-stored-with-their-seq.md -->

Every message is one JSON object in one text frame. Unknown `type`s and unknown fields are
refused with an `error`, not ignored. Room ids are canonical lowercase UUIDs.

Client to server:

| `type` | Fields | Meaning |
|---|---|---|
| `join` | `room`; optional `after` (seq), `delivery` (`"durable"`, the default, or `"lossy"`), `kind` (`"group"`, the default, `"direct"` or `"live"`) | Subscribe this connection to the room. Joining an unknown room creates it, as the closed `kind` it names; `"live"` joins only a room the server opened (see [Member lists](#member-lists)). With `after`, the node also sends what it still holds above that seq (see [Resume and history](#resume-and-history)). |
| `send` | `room`, `id`, `body` | Post a message, once the room's `joined` has arrived; before it, the send is refused with `not_joined`. `id` is 1 to 64 characters of `A-Z a-z 0-9 _ -`, unique per sender and room: use a UUID or ULID per message. `body` is the message's bytes in base64url without padding (RFC 4648 section 5). |
| `history` | `room`; optional `before` or `after` (a seq, not both), `limit` (1 to 100, default 50) | A page of the room's stored messages. Without a cursor, or with `before`, newest first below it; with `after`, oldest first above it. Only once the room's `joined` has arrived; before it, `not_joined`. |

Server to client:

| `type` | Fields | Meaning |
|---|---|---|
| `joined` | `room`, `seq` | The join succeeded. `seq` is the room's latest seq: a client whose last seq is lower missed messages. |
| `sent` | `room`, `id`, `seq` | The message was sequenced as `seq`. A resend with the same `id` gets the same answer. |
| `message` | `room`, `seq`, `sender`, `id`, `body` | A message in the room, your own included, live, resumed or from history. `sender` is the poster's user id ([auth.md](auth.md)). |
| `history` | `room`, `count` | Ends the answer to a `history` command, after its `count` messages. `0`: nothing more in that direction. |
| `error` | `reason`, plus `room` and `id` when known, `retry_after_ms` for `rate_limited` | A command failed. |

```json
{"type":"join","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","after":41}
{"type":"joined","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","seq":44}
{"type":"send","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","id":"01J9ZQ4V7B8K3M2N5P6R7S8T9W","body":"aGVsbG8"}
{"type":"sent","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","id":"01J9ZQ4V7B8K3M2N5P6R7S8T9W","seq":45}
{"type":"message","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","seq":45,"sender":"user-42","id":"01J9ZQ4V7B8K3M2N5P6R7S8T9W","body":"aGVsbG8"}
{"type":"history","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","before":42,"limit":20}
{"type":"history","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","count":20}
```

`body` is opaque to the service: any bytes, plaintext or ciphertext, carried, stored and returned
exactly as sent, never parsed, logged or indexed. Within a room, `seq` rises by one per message
and gives every client the same total order. Every seq is stored with its message before anyone
is sent it, so history has every seq that was ever delivered.

**Resends.** After `unavailable`, send the same message again with the same `id`, on this or any
connection, to any node: it is sequenced once, answered with the first seq, and delivered once.
Reusing an `id` for a different message is refused with `conflict`: nothing is sequenced or
delivered, and the `id` stays with the first message. Send the new message under a new `id`.

### Resume and history

<!-- apps/chat/src/chat_service.cpp (replay, history, page_read) -->

- **A page** is the `count` `message` frames that come immediately before its `history` frame:
  the service sends them together, with nothing between them. Other messages of the room may
  arrive before or after the page, never inside it.
- **Resume.** Rejoin with `"after"` set to the last seq you have. The node sends what it still
  keeps above it (up to 128 KiB of the newest), then live messages. It keeps a room's latest
  messages only while it is in the room and for 30 s after its last client left.
- **Gaps.** If `joined`'s `seq`, or the next message's, is more than one above the last seq you
  have, fill the gap with `history` and `"after"`, page after page, until a page's `count` is
  `0` or you reach what you already hold. A client applying MLS commits applies none past a gap
  until it is filled.
- **Scrolling back.** `history` without a cursor gives the newest messages; each next page is
  `"before"` the lowest seq you got. A page ends early, with fewer than `limit` messages, when
  the connection already has much unread output (a page queues at most 128 KiB behind it);
  ask again from where it stopped. When it cannot send even one message it answers `busy`.
- **Cost.** A `history` command, and a rejoin with `after`, count against the same allowance as
  joins: a burst of 64, then one a second.

### Member lists

<!-- apps/chat/src/chat_service.cpp (join, admitted), infra/postgres/src/message_sql.hpp (kAdmits, kRecordLive), migrations/0005_chat_messages.sql (chat_members, chat_rooms) -->

Who may join a room depends on its kind, which is recorded once and never changes:

- **Direct and group chats** (`"kind":"direct"` or `"group"`, the default) admit only their
  members. Anyone else's `join` is refused with `not_member`, so they can neither send to the
  room nor read its history. A direct or group chat with no members admits nobody. The first
  join of a room with no kind recorded records the kind it names; so does listing its first
  member (as a group chat).
- **A stream's live chat** admits anyone. Only the server opens one, before anyone joins it; a
  client cannot. A join that says `"kind":"live"` is admitted in a room the server opened, and
  refused with `not_live` in any other, which it leaves as it was. Joins of a live room need
  not name the kind.

No client command changes a member list; they are set by the service's operators, and later by
the product, in the database. A member removed from the list keeps receiving the room's
messages, and can read its history, until that connection closes; the next `join` is refused.

### Errors

| `reason` | Meaning | Client action |
|---|---|---|
| `not_json` | The frame is not JSON | Fix the client |
| `malformed` | Not an object, unknown or missing `type`, missing or unknown field, a value of the wrong kind, both `before` and `after`, a `limit` out of range | Fix the client |
| `bad_room` | `room` is not a canonical lowercase UUID | Fix the client |
| `bad_id` | `id` is not a message id | Fix the client |
| `bad_body` | `body` is not base64url | Fix the client |
| `not_member` | The room has a member list without you | Do not retry |
| `not_live` | `"kind":"live"` for a room the server has not opened as a stream's live chat | Do not retry; join without `kind` if it is a group chat you are a member of |
| `not_joined` | `send` or `history` for a room this connection has not joined | Join first |
| `too_many_rooms` | This connection already holds 64 rooms | Use another connection, or leave some rooms by reconnecting |
| `rate_limited` | Past the send allowance; `retry_after_ms` says when one more is allowed | Wait that long; the message was neither sequenced nor delivered |
| `busy` | Join or history allowance exceeded, too many sends awaiting answers, the room's owner queue is full, or too much unread output for a history page | Back off and retry |
| `unavailable` | The room's owner or the store could not be reached | Retry; resend a `send` with the same `id` |
| `fenced` | The room changed owners while the write was in flight | Retry with the same `id` |
| `conflict` | This `id` was already used for a different message in the room | Send it under a new `id` |

## Limits

<!-- apps/chat/src/chat.hpp (Limits), codec/ws/include/codec/ws/decoder.hpp, apps/chat/src/session.cpp -->

| Limit | Value | On breach |
|---|---|---|
| Message size | 64 KiB per message (after reassembly) | Close `1009` |
| Binary frames | Not accepted | Close `1003` |
| Control frames | 8 per read; token bucket of 20, refilling 10/s | Close `1008` |
| Rooms per connection | 64 | `error` `too_many_rooms` |
| New-room joins per user | Burst 64, then 1/s, across all the user's connections on a node | `error` `busy` |
| Sends | Burst 10, then 2/s, per user across the user's connections on a node | `error` `rate_limited` with `retry_after_ms` |
| Sends awaiting an answer | 128 KiB per connection, each counted as body + 256 bytes | `error` `busy` |
| Resends recognised | For about a minute on the node that sequenced or delivered them; always, by the store, once sequenced | |
| Resume and history output | 128 KiB queued behind a connection's unread output | Shorter page, or `busy` |
| History pages and resumes | Counted with joins: burst 64, then 1/s per user | `error` `busy` |
| Lossy delivery | Skipped while more than 64 KiB behind | Gap in seqs; fill from history |
| Unread output | 256 KiB per connection | Connection closed; reconnect and rejoin |
| Handshake | 10 s from accept to a complete upgrade request | Connection closed |
| Idle | The server pings after 30 s of silence and closes after 75 s with nothing received | Answer pings (browsers do this themselves) |
| Server drain | Close `1001`, then 5 s | Reconnect |

## Presence

<!-- apps/chat/src/presence.hpp (PresenceLimits), apps/chat/src/envelope.hpp, apps/chat/src/session.cpp (command), docs/adr/0056-presence-over-the-room-plane.md -->

A client can watch other users and hear when they come online and go offline. A user is online
while they have at least one open socket to any chat node, and for a grace of 10 s after their
last one closes: a page reload or a reconnect, even through another node, within the grace is
never reported. Nothing needs to be joined first.

Client to server:

| `type` | Fields | Meaning |
|---|---|---|
| `watch` | `user` | Hear this user's presence on this connection. Watching the same user again is answered again. |
| `unwatch` | `user` | Stop. Not answered; unwatching a user not watched does nothing. |

Server to client:

| `type` | Fields | Meaning |
|---|---|---|
| `watching` | `user`, `status` (`online` or `offline`) | The answer to `watch`: what the node knows now. |
| `presence` | `user`, `status` (`online` or `offline`) | The user's status changed. Sent once per change, to every connection watching them. |
| `error` | `reason`, `user` | The watch was refused (below). |

```json
{"type":"watch","user":"user-42"}
{"type":"watching","user":"user-42","status":"offline"}
{"type":"presence","user":"user-42","status":"online"}
{"type":"presence","user":"user-42","status":"offline"}
```

`user` is the watched user's id as it appears in `sender` ([auth.md](auth.md)). A `watching`
that says `offline` may be followed within a round trip by `presence` `online`: the node had not
yet heard from the node the user is connected through. Treat `watching` as the starting state
and apply each `presence` in order.

When a user goes offline:

- after closing their last socket normally: 10 s later (the grace);
- when their connection dies without a close: once the server notices, at most 75 s later (the
  idle timeout in [Limits](#limits)), plus the grace;
- when the node they were on stops (a crash, or a deploy draining it) and they do not reconnect:
  up to 150 s later.

Watches last as long as the connection. After a reconnect, watch again.

Errors for `watch` and `unwatch`:

| `reason` | Meaning | Client action |
|---|---|---|
| `malformed` | Missing `user`, or another field | Fix the client |
| `bad_user` | `user` is not a user id | Fix the client |
| `watching_self` | `user` is the connection's own user | Nothing to watch: the connection is online |
| `too_many_watches` | This connection already watches 128 users | Unwatch some first |
| `busy` | This node watches as many users as it takes, or this user started watching users no connection on this node was watching (unwatching and watching again counts each time) faster than 128 at once and then 1 a second | Back off and retry |

Room ids of UUID version 8 (the third group starts with `8`) whose first byte is `02` (the id
starts with `02`) are reserved for presence: `join`, `send` or `history` naming one is refused
with `bad_room`. Presence events are never stored.

Anyone signed in may watch anyone: there is no check of who may see whose presence yet. That is
an open item before production ([ADR-0056](../adr/0056-presence-over-the-room-plane.md)).

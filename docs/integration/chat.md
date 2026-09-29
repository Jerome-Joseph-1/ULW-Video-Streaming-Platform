# Chat

> **Draft: changes until milestone M19 merges.** What follows is the protocol on `main` today
> (M16). M17 replaces the message envelope (see [Coming in M17](#coming-in-m17)), M18 adds
> [presence](#presence) and M19 adds history and membership checks. Nothing here is a
> compatibility promise yet.

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

## Messages as of main (M16)

<!-- apps/chat/src/envelope.hpp, apps/chat/src/envelope.cpp, apps/chat/src/session.cpp -->

Every message is one JSON object in one text frame. Unknown `type`s and unknown fields are
refused with an `error`, not ignored. Room ids are canonical lowercase UUIDs.

Client to server:

| `type` | Fields | Meaning |
|---|---|---|
| `join` | `room` | Subscribe this connection to the room. Joining an unknown room creates it. |
| `send` | `room`, `body` (string), optional `ref` (non-negative integer) | Post a message. `ref` is echoed in the answer so the client can match it. |

Server to client:

| `type` | Fields | Meaning |
|---|---|---|
| `joined` | `room` | The join succeeded; messages for the room follow. |
| `sent` | `room`, `seq`, and `ref` if the send had one | The message was sequenced as `seq` in that room. |
| `message` | `room`, `seq`, `sender`, `body` | A message in the room, including your own. `sender` is the poster's user id ([auth.md](auth.md)). |
| `error` | `reason`, plus `room` and `ref` when known | A command failed. |

```json
{"type":"join","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d"}
{"type":"joined","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d"}
{"type":"send","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","ref":1,"body":"hello"}
{"type":"sent","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","ref":1,"seq":7}
{"type":"message","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","seq":7,"sender":"user-42","body":"hello"}
```

`body` is opaque to the service: it is carried and returned byte for byte (re-escaped as JSON),
never parsed, logged or indexed. Within a room, `seq` gives every client the same total order.

`error` reasons:

| `reason` | Meaning | Client action |
|---|---|---|
| `not_json` | The frame is not JSON | Fix the client |
| `malformed` | Not an object, unknown or missing `type`, missing field, unknown field, wrong value type | Fix the client |
| `bad_room` | `room` is not a canonical lowercase UUID | Fix the client |
| `not_joined` | `send` to a room this connection has not joined | Join first |
| `too_many_rooms` | This connection already holds 64 rooms | Use another connection, or leave some rooms by reconnecting |
| `busy` | Join rate exceeded, or too many sends awaiting answers, or the room's owner queue is full | Back off and retry |
| `unavailable` | The room's owner could not be reached | Retry; the send may or may not have been sequenced (M16 has no dedupe) |
| `fenced` | The room changed owners while the write was in flight | Retry |

## Limits

<!-- apps/chat/src/chat.hpp (Limits), codec/ws/include/codec/ws/decoder.hpp, apps/chat/src/session.cpp -->

| Limit | Value | On breach |
|---|---|---|
| Message size | 64 KiB per message (after reassembly) | Close `1009` |
| Binary frames | Not accepted | Close `1003` |
| Control frames | 8 per read; token bucket of 20, refilling 10/s | Close `1008` |
| Rooms per connection | 64 | `error` `too_many_rooms` |
| New-room joins per user | Burst 64, then 1/s, across all the user's connections on a node | `error` `busy` |
| Sends awaiting an answer | 128 KiB per connection, each counted as body + 256 bytes | `error` `busy` |
| Unread output | 256 KiB per connection | Connection closed; reconnect and rejoin |
| Handshake | 10 s from accept to a complete upgrade request | Connection closed |
| Idle | The server pings after 30 s of silence and closes after 75 s with nothing received | Answer pings (browsers do this themselves) |
| Server drain | Close `1001`, then 5 s | Reconnect |

M16 has no resume: after a reconnect a client rejoins its rooms and receives only new messages.

## Coming in M17

<!-- lane/m17-chat: apps/chat/src/envelope.hpp, docs/adr/0049-chat-service-policy-between-edge-and-rooms.md -->

M17 replaces the envelope. It is not on `main` yet; build against it only once it merges. The
shapes as they stand on the M17 branch:

Client to server:

```json
{"type":"join","room":"<uuid>","after":41,"delivery":"durable"}
{"type":"send","room":"<uuid>","id":"<message id>","body":"<base64url>"}
```

Server to client:

```json
{"type":"joined","room":"<uuid>"}
{"type":"sent","room":"<uuid>","id":"<message id>","seq":42}
{"type":"message","room":"<uuid>","seq":42,"sender":"<sub>","id":"<message id>","body":"<base64url>"}
{"type":"error","reason":"rate_limited","room":"<uuid>","id":"<message id>","retry_after_ms":500}
```

What changes:

- **Bodies are bytes**, sent as base64url without padding (RFC 4648 section 5), so ciphertext can
  be carried. A body that is not base64url is refused with `bad_body`.
- **Message ids replace `ref`.** Every `send` carries an `id` of 1 to 64 characters from
  `A-Z a-z 0-9 _ -`, unique per sender and room (use a UUID). A resend with the same id is
  answered with the first send's `seq` and delivered once, for about a minute. A bad id is refused
  with `bad_id`. Reusing an id for a different message loses the second one.
- **Acks.** `sent` carries the id and the seq.
- **Resume.** `join` with `"after":N` delivers, after `joined`, the messages this node still
  holds above `N` (up to 128 KiB of the newest), then live ones. Seqs rise by one per message; a
  jump is a gap, to be filled from history once M19 provides it.
- **Delivery mode.** `"delivery":"lossy"` on `join` skips messages while the connection is more
  than 64 KiB behind instead of closing it. `"durable"` is the default.
- **Rate limit.** 10 sends, refilling 2 per second, per user per node. Past it the send is
  answered `error` with `reason` `rate_limited` and `retry_after_ms`, and is neither sequenced nor
  delivered.

## Presence

<!-- apps/chat/src/presence.hpp (PresenceLimits), apps/chat/src/envelope.hpp, apps/chat/src/session.cpp (command), docs/adr/0053-presence-over-the-room-plane.md -->

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
| `too_many_watches` | This connection already watches 128 users | Unwatch some first |
| `busy` | This node watches as many users as it takes, or this user started watching users nobody on this node watched faster than 128 at once and then 1 a second | Back off and retry |

Room ids of UUID version 8 (the third group starts with `8`) are reserved for presence: `join`
or `send` naming one is refused with `bad_room`.

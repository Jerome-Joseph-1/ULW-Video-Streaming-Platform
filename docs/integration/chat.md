# Chat

> **Draft until phase 2 is tagged.** Messages, acks, resume, history and member lists below are
> what `main` does. M18 adds presence messages; nothing else here is expected to change before
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

<!-- apps/chat/src/envelope.hpp, apps/chat/src/envelope.cpp, apps/chat/src/chat_service.cpp, docs/adr/0043-chat-service-policy-between-edge-and-rooms.md, docs/adr/0052-messages-stored-with-their-seq.md -->

Every message is one JSON object in one text frame. Unknown `type`s and unknown fields are
refused with an `error`, not ignored. Room ids are canonical lowercase UUIDs.

Client to server:

| `type` | Fields | Meaning |
|---|---|---|
| `join` | `room`; optional `after` (seq), `delivery` (`"durable"`, the default, or `"lossy"`) | Subscribe this connection to the room. Joining an unknown room creates it. With `after`, the node also sends what it still holds above that seq (see [Resume and history](#resume-and-history)). |
| `send` | `room`, `id`, `body` | Post a message. `id` is 1 to 64 characters of `A-Z a-z 0-9 _ -`, unique per sender and room: use a UUID or ULID per message. `body` is the message's bytes in base64url without padding (RFC 4648 section 5). |
| `history` | `room`; optional `before` or `after` (a seq, not both), `limit` (1 to 100, default 50) | A page of the room's stored messages. Without a cursor, or with `before`, newest first below it; with `after`, oldest first above it. The connection must have joined the room. |

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

<!-- apps/chat/src/chat_service.cpp (join, admitted), migrations/0005_chat_messages.sql (chat_members) -->

A room with a member list admits only its members: anyone else's `join` is refused with
`not_member`, and so they can neither send to it nor read its history. A room with no member
list is open to anyone, which is every room until one is listed, and what a stream's live chat
stays. No client command changes a member list; they are set by the service's operators, and
later by the product, in the database.

### Errors

| `reason` | Meaning | Client action |
|---|---|---|
| `not_json` | The frame is not JSON | Fix the client |
| `malformed` | Not an object, unknown or missing `type`, missing or unknown field, a value of the wrong kind, both `before` and `after`, a `limit` out of range | Fix the client |
| `bad_room` | `room` is not a canonical lowercase UUID | Fix the client |
| `bad_id` | `id` is not a message id | Fix the client |
| `bad_body` | `body` is not base64url | Fix the client |
| `not_member` | The room has a member list without you | Do not retry |
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

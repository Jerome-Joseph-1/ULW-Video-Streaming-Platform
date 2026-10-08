# Chat

> **Stable.** The WebSocket envelope (messages, acks, resume, history, member lists, the
> [commands that change them](#changing-member-lists) and [presence](#presence)) and
> [the service API](#the-service-api) are kept under the compatibility rules in
> [versioning.md](versioning.md). End-to-end encrypted rooms follow [e2ee.md](e2ee.md), which is
> still a draft.

Chat is its own service, `chat_server`, separate from the video gateway (ADR-0019). Clients hold
one WebSocket to it and send JSON messages in text frames.

## Connecting

<!-- apps/chat/src/session.cpp (route, answer_request), apps/chat/src/config.cpp, docs/adr/0036-chat-server-client-edge.md -->

| | |
|---|---|
| Endpoint | `GET /rt` with a WebSocket upgrade (RFC 6455, version 13) on the chat host: `wss://<CHAT_HOST>/rt` |
| Subprotocols, extensions | None. `permessage-deflate` is not offered. |
| Auth | The identity provider's token, as for the gateway ([auth.md](auth.md)): `Authorization: Bearer <token>`, or the `ULW_AUTH_COOKIE` cookie |
| Cookie and `Origin` | A cookie token is accepted only when the request's `Origin` is listed exactly in `ULW_ALLOWED_ORIGINS` (`scheme://host[:port]`, comma separated; `http://` only for `localhost`, `127.0.0.1` or `[::1]`, and no explicit default port such as `:443`). With no list configured, cookies are refused. A bearer token is accepted from any origin. |

Upgrade refusals (the connection is closed after the response, and the body is empty):

| Status | When |
|---|---|
| `400` | Malformed upgrade or `Sec-WebSocket-Key`, or the request carries a body |
| `426` with `Upgrade: websocket` | `GET /rt` that is not an upgrade, or a WebSocket version other than 13 |
| `401` | No usable token, or the token fails verification |
| `403` | Cookie token without an allowed `Origin` |
| `429` with `Retry-After` | Behind a trusted proxy, the forwarded address already has `ULW_MAX_CONNECTIONS_PER_IP` (20) upgrades waiting for an answer (`Retry-After: 1`); or the user already has `ULW_MAX_SESSIONS_PER_USER` (16) sockets open on this node (`Retry-After: 5`). Retry after that long, or close a socket you no longer use |
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
| `join` | `room`, or `stream` for a live stream's chat; optional `after` (seq), `delivery` (`"durable"`, the default, or `"lossy"`), `kind` (`"group"`, the default, or `"direct"`; not with `stream`) | Subscribe this connection to the room. Joining an unknown room creates it, as the closed `kind` it names (see [Member lists](#member-lists)). `stream` names a live stream as its playback URL does, and joins its chat (see [A stream's live chat](#a-streams-live-chat)). With `after`, the node also sends what it still holds above that seq (see [Resume and history](#resume-and-history)). |
| `send` | `room`, `id`, `body` | Post a message, once the room's `joined` has arrived; before it, the send is refused with `not_joined`. `id` is 1 to 64 characters of `A-Z a-z 0-9 _ -`, unique per sender and room: use a UUID or ULID per message. `body` is the message's bytes in base64url without padding (RFC 4648 section 5). |
| `history` | `room`; optional `before` or `after` (a seq, not both), `limit` (1 to 100, default 50) | A page of the room's stored messages. Without a cursor, or with `before`, newest first below it; with `after`, oldest first above it. Only once the room's `joined` has arrived; before it, `not_joined`. |
| `call` | `room`, `device` (a UUID the client keeps per device); `answer` (the ringing call it answers), optional | A ticket to the room's call, for a direct or group chat this connection has joined. See [calls.md](calls.md). |
| `call_decline`, `call_cancel`, `call_end` | `room`, `call` | Turn a ringing call down (a callee), give up ringing (the caller), or end an answered call (either member of a direct chat; a group call's caller, for everyone), for a room this connection has joined. See [calls.md](calls.md#ringing). |
| `call_leave` | `room`, `call` | Leave a group call, which goes on for the others. See [calls.md](calls.md#group-calls). |
| `call_expel` | `room`, `call`, `user` | A group call's caller puts `user` out of the call. See [calls.md](calls.md#group-calls). |
| `open_direct`, `create_group`, `add_members`, `remove_member`, `leave`, `rooms`, `members` | See [Changing member lists](#changing-member-lists) | A user's direct and group chats, and who is in them. |

Server to client:

| `type` | Fields | Meaning |
|---|---|---|
| `joined` | `room`, `seq` | The join succeeded. `seq` is the room's latest seq: a client whose last seq is lower missed messages. |
| `sent` | `room`, `id`, `seq` | The message was sequenced as `seq`. A resend with the same `id` gets the same answer. |
| `message` | `room`, `seq`, `sender`, `id`, `body` | A message in the room, your own included, live, resumed or from history. `sender` is the poster's user id ([auth.md](auth.md)). |
| `history` | `room`, `count` | Ends the answer to a `history` command, after its `count` messages. `0`: nothing more in that direction. |
| `ticket` | `room`, `url`, `token`, `expires_at`, `call` when the ticket belongs to a call | The answer to `call`: connect LiveKit's SDK to `url` with `token` before `expires_at` (Unix seconds). See [calls.md](calls.md). |
| `call_ringing`, `call_answered`, `call_declined`, `call_cancelled`, `call_missed`, `call_ended`, `call_left`, `call_moved` | `room`, `call`, `from`; `expires_at` (ringing), `by` (who did it, when someone did), `expelled` (moved) | Unasked, on every socket of the call's members, joined to the room or not: a call rings, or how it went. See [calls.md](calls.md#ringing) and [calls.md](calls.md#group-calls). |
| `direct`, `group`, `added`, `removed`, `left`, `rooms`, `members`, `member` | See [Changing member lists](#changing-member-lists) | Answers to the member-list commands, and `member`, sent unasked when a list you are on, or of a room you joined, changes. |
| `error` | `reason`, plus `room` and `id` when known, `retry_after_ms` for `rate_limited` and for a call's `unavailable` | A command failed. |

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

<!-- apps/chat/src/chat_service.cpp (join, admitted), infra/postgres/src/message_sql.hpp (kAdmits, kRecordLive), migrations/0005_chat_messages.sql (chat_members, chat_rooms), migrations/0010_chat_rooms_recorded_at.sql, infra/postgres/src/upload_reaper.cpp (kForgetUnused) -->

Who may join a room depends on its kind, which is recorded once and never changes while the
room is in use (a room nothing used may be forgotten, below):

- **Direct and group chats** (`"kind":"direct"` or `"group"`, the default) admit only their
  members. Anyone else's `join` is refused with `not_member`, so they can neither send to the
  room nor read its history. A direct or group chat with no members admits nobody. The first
  join of a room with no kind recorded records the kind it names; so does listing its first
  member (as a group chat). Such a join is refused with `not_member`, since the room lists
  nobody yet. Each user's refused joins record at most 20 rooms at once and then one a minute,
  per chat node; past that a join is refused the same and records nothing. A direct or group
  chat recorded more than a day ago and never used (no members, never on the room plane) is
  forgotten, at the latest within a few reaper passes; the next join of it records it again, as
  the kind that join names, which may be the other closed kind. A stream's live chat is never
  forgotten.
- **A stream's live chat** admits anyone. Only the server opens one, and only a stream's room
  can be one; a client cannot. It is joined by the stream's name (see
  [A stream's live chat](#a-streams-live-chat)), and refused with `not_live` until it is open.

Members change their lists with the commands below ([Changing member lists](#changing-member-lists));
operators may still change them in the database (RUNBOOK). A member removed from the list,
however, is taken out of the room at once on every socket they have, on every node (ADR-0073): each gets an `error` with `not_member` for
the room, unasked, and receives nothing more from it; `send` and `history` there answer
`not_joined`, and the next `join` is refused. After a node lost track of removals for a while it
checks every member list its sockets rely on again; a list it cannot read for a reason other
than an outage takes the socket out of the room the same way, but with `unavailable`, since the
list never said no: join again.

### Changing member lists

<!-- apps/chat/src/membership.cpp, apps/chat/src/named_rooms.cpp, apps/chat/src/envelope.cpp, apps/chat/src/config.cpp (ULW_CHAT_SELF_SERVICE), infra/postgres/src/message_sql.hpp (kOpenDirect, kCreateGroup, kAddMembers, kExpel, kLeave, kRoomsFirst, kRoster), migrations/0015_chat_membership.sql, docs/adr/0096-member-lists-changed-by-their-users.md -->

A signed-in user manages their direct and group chats on the same WebSocket. None of these
commands needs a `join` first; join the room afterwards to send and read it. Every answer goes to
the connection that asked; what changed reaches the users concerned on their own sockets, on
every node, as an unasked `member` frame.

**Who may start a conversation is the operator's choice** (`ULW_CHAT_SELF_SERVICE`,
[operator-contract.md](operator-contract.md)). Off, the default, users cannot reach someone just
by knowing their id: `open_direct`, `create_group` and `add_members` are answered `not_allowed`,
and the embedding product lists people through [the service API](#the-service-api) once its own
request-and-accept step allows it. `leave`, an admin's `remove_member`, `rooms` and `members` work
either way. On, users do all of it themselves, as the commands below describe; that suits a demo
or a closed community, where anyone signed in may reach anyone.

Client to server:

| `type` | Fields | Meaning |
|---|---|---|
| `open_direct` | `user` | The direct chat of you and `user`. The first time, it lists you both; after that it is the same room, whichever of you asks, and changes nothing. With self-service off: `not_allowed`. |
| `create_group` | `id`, optional `users` (at most 50) | A group chat with you as its admin and `users` as its members. `id` is a request id, as a message's (1 to 64 of `A-Z a-z 0-9 _ -`): the same `id` again, after `unavailable` say, names the same room and lists nobody more. Use a new `id` per group. With self-service off: `not_allowed`. |
| `add_members` | `room`, `users` (1 to 50) | Lists `users` in a group chat you are an admin of. Those listed already are left as they are. With self-service off: `not_allowed`. |
| `remove_member` | `room`, `user` | Takes `user` off a group chat you are an admin of. Naming yourself is `leave`. |
| `leave` | `room` | Takes you off a group chat's list. When its last admin leaves, the remaining member whose id sorts first (bytewise) becomes its admin, and `left` names them. Your sockets that joined the room leave it as for any removal (`error` `not_member`, then `member`), before or after the answer. |
| `rooms` | optional `after` (a room id), `limit` (1 to 100, default 50) | The rooms you are listed in, in room id order (bytewise, not by activity); the next page is `after` the last one. |
| `members` | `room`, optional `after` (a user id), `limit` (1 to 100, default 50) | The room's members and their roles, by user id; only for a member. |

Server to client:

| `type` | Fields | Meaning |
|---|---|---|
| `direct` | `room`, `user` | The answer to `open_direct`. |
| `group` | `room`, `id` | The answer to `create_group`. |
| `added` | `room`, `users` | The answer to `add_members`: who it listed, not those listed already. |
| `removed` | `room`, `user` | The answer to `remove_member`. |
| `left` | `room`, `promoted` when you were its last admin | The answer to `leave`: `promoted` is the member who became the group's admin. |
| `rooms` | `rooms` (objects of `room`, `kind`: `direct`, `group` or `live`; `role`: `member` or `admin`; and `peer`, the other member of a direct chat), `more` | A page of your rooms; `more` is `true` when another follows. |
| `members` | `room`, `members` (objects of `user` and `role`), `more` | A page of the room's members. |
| `member` | `room`, `user`, `change` (`added`, `removed`, `promoted` or `demoted`) | Unasked: `user` was listed in, taken off, made admin of, or made a plain member of the room. Sent to every socket of `user` and to every socket that joined the room, however the list changed (a command, or an operator). A socket whose join of the room is still waiting for its answer hears only of its own user. |
| `error` | `reason`, plus `room`, `id` (of a `create_group`) and `user` when the command named them, `retry_after_ms` for `rate_limited` | The command changed nothing. |

As `user-1`:

```json
{"type":"open_direct","user":"user-42"}
{"type":"direct","room":"03db1b2d-a8da-8387-a668-3abc70b6953e","user":"user-42"}
{"type":"create_group","id":"01J9ZQ4V7B8K3M2N5P6R7S8T9W","users":["user-42","user-7"]}
{"type":"group","room":"04ff0817-879c-8a63-b5a8-9ebacad5d927","id":"01J9ZQ4V7B8K3M2N5P6R7S8T9W"}
{"type":"member","room":"04ff0817-879c-8a63-b5a8-9ebacad5d927","user":"user-1","change":"added"}
{"type":"rooms","rooms":[{"room":"03db1b2d-a8da-8387-a668-3abc70b6953e","kind":"direct","role":"member","peer":"user-42"},{"room":"04ff0817-879c-8a63-b5a8-9ebacad5d927","kind":"group","role":"admin"}],"more":false}
```

- **Rooms are named by what they are for.** A direct chat's room id is derived from its two user
  ids, a group's from its creator and the `create_group` `id`. Take SHA-256 over the name: for a
  direct chat, `ulw direct chat`, a newline, then the two user ids in bytewise order with a
  newline between them (no newline at the end); for a group, `ulw group chat`, a newline, the
  creator's id, a newline and the request's `id`. Keep the digest's first 16 bytes, replace the
  first of them with the tag (`03` direct, `04` group), then set the version and variant bits as
  RFC 9562 does for version 8: the high four bits of byte 6 become `1000`, the high two bits of
  byte 8 become `10`. So the id is the tag followed by bytes 1 to 15 of the digest, not by its
  first 15 (unlike a stream's chat, below). Alice and Bob's room (`alice`, `bob`): the digest
  begins `952768cd63d33415bc35024bab6c3653`, and the room is
  `032768cd-63d3-8415-bc35-024bab6c3653`. The same ids name the same room whether a user's
  command or [the service API](#the-service-api) lists it. Use the room the answer names rather
  than computing it.
  A `join` of such a room asks for its kind whatever it says, and is `bad_room` if `"kind"`
  names the other.
- **Who you name.** A user id is the identity provider's subject for that person
  ([auth.md](auth.md)). The service has no list of users, so it does not check that someone
  exists: resolve people to ids in your own directory, and never send a name typed by the user.
- **Roles.** A group's creator is its admin; everyone else is a member, and so are both people
  of a direct chat. Only an admin adds or removes others (`not_admin`). A direct chat's pair never
  changes: `add_members`, `remove_member` and `leave` on one are `not_group`, so neither of the
  two can leave it alone; ending one (an unfriend, a block) is the product's, through the service
  API's `close_direct`, which unlists both. A member an operator removed from a direct chat is
  not put back when the other opens it again.
- **Self-service on means anyone can reach anyone.** With `ULW_CHAT_SELF_SERVICE=on`, any
  signed-in user can `open_direct` anyone whose id they know, and so watch their presence and
  call them; and since `open_direct` lists a pair again once a closed chat lists nobody, nothing
  keeps a closed (blocked) pair apart. Run it on only where that is acceptable.
- **Size.** A group holds at most 100 members; an `add_members` that would pass that adds
  nobody (`too_many_members`). Once you are listed in 1000 rooms, `open_direct` and
  `create_group` of a new room answer `room_limit` (others can still add you); leave some first.
- **A group's `id` is used once.** When everyone has left a group that holds messages, a
  `create_group` under its `id` answers `gone` and lists nobody, so that new members never get
  the old history; use a new `id`.
- **Allowance.** `open_direct`, `create_group`, `add_members`, `remove_member` and `leave` share
  one allowance per user on each node (a user connected to several nodes has one on each): 20 at
  once, then one each 3 s; past it they are
  `rate_limited` with `retry_after_ms`. `rooms` and `members` count with joins and history.
- **End-to-end encrypted rooms.** The server's list decides who may join, send and read; the MLS
  group is your devices' (ADR-0016), and the server never reads a commit. When you add members,
  fetch a KeyPackage for each of their devices from the key directory and send the Commit and
  the Welcome as ordinary messages in the room; the new members join and read the Welcome from
  history. When a `member` frame says someone was removed or left, one remaining member's device
  commits their removal (someone who leaves cannot remove themselves); the first valid Commit
  for an epoch in `seq` order wins ([e2ee.md](e2ee.md)). Compare the MLS roster with `members`
  and treat a difference as a commit owed. A removed member receives nothing from the room from
  the moment of removal, before any commit.

### A stream's live chat

<!-- apps/chat/src/live_chat.cpp, apps/chat/src/envelope.cpp (joined_room_of), apps/chat/src/chat_service.cpp (subscribe, send, delivered, catch_up), migrations/0008_live_chat_room.sql, infra/postgres/src/room_store.cpp (kAppendMessage), docs/adr/0070-live-chat-lossy-and-bounded.md -->

- **Joining.** `{"type":"join","stream":"show-1"}`, with the stream's name as the live packager
  and the playback URL (`live/<stream>/`) have it: 1 to 64 of `A-Z a-z 0-9 _ -`, else
  `bad_stream`. `joined` names the room; sends and `history` use that room id like any other.
  Its id is a version 8 UUID: the byte `0x01`, then the first 15 bytes of SHA-256 over
  `ulw-live-chat:` and the name, with the version and variant bits set (`show-1` is
  `011b9ed0-d6b6-88e6-ac34-32d7070ba83b`), and a `join` that gives such an id as
  `room` is refused with `bad_room`: a live chat is joined only by its stream.
- **Open once the stream's chat is opened.** The server side records a stream's room live
  before viewers join (RUNBOOK, "Chat rooms"). Until then a stream join is refused with
  `not_live`; retry when the stream is on air. Once open it admits anyone signed in, until the
  stream ends: then it closes to new joins (`not_live` again), while sockets already in it stay
  until they leave (ADR-0092).
- **Always lossy.** Every viewer is lossy, whatever its `delivery`, and is never closed for being
  behind. A viewer more than 64 KiB behind is sent nothing new until its connection has caught
  up; then it is sent what it missed, oldest first, but only the newest 64 messages of it. The
  seqs show everything else as a gap: a viewer never sees a message twice or out of order.
  Fill the gap from `history` if you want it; a live chat usually does not.
- **Sending.** A live chat message's `body` is at most 2000 bytes (`too_large` beyond). Besides
  each user's own allowance, each chat node lets 40 messages into a live chat at once, then 20
  a second, from all its senders together; past it the send is `rate_limited` with
  `retry_after_ms`, and costs the sender nothing of their own allowance.
- **History** keeps the chat's newest 1000 messages; older ones are deleted as new ones are
  stored, and a page below them is empty. A resend under an `id` whose message is already that
  old is stored again, as a new message.
- A viewer whose connection acknowledges nothing the server sent it for 20 s is disconnected
  by the server, with a reset (counted in `stalled_readers_total`). A client that keeps
  reading, however slowly, is not. Reconnect and join the stream again.

### Errors

| `reason` | Meaning | Client action |
|---|---|---|
| `not_json` | The frame is not JSON | Fix the client |
| `malformed` | Not an object, unknown or missing `type`, missing or unknown field, a value of the wrong kind, both `before` and `after`, a `limit` out of range, `users` empty for `add_members` or longer than 50 | Fix the client |
| `bad_room` | `room` (or a `rooms` `after`) is not a canonical lowercase UUID, is a stream's chat room given to `join`, or a direct or group chat's room joined with `"kind"` naming the other | Fix the client; join a stream's chat by `stream` |
| `bad_id` | `id` is not a message id | Fix the client |
| `bad_body` | `body` is not base64url | Fix the client |
| `bad_stream` | `stream` is not a stream name | Fix the client |
| `bad_device` | A call's `device` is not a canonical lowercase UUID | Fix the client |
| `bad_user` | A `user`, an entry of `users` or a `members` `after` is not a user id | Fix the client |
| `self` | `open_direct` with your own user id | Nothing to open |
| `not_allowed` | `open_direct`, `create_group` or `add_members` where the operator turned self-service off (`ULW_CHAT_SELF_SERVICE`, the default) | Do not retry; ask the product, whose backend lists people ([The service API](#the-service-api)) |
| `not_admin` | `add_members` or `remove_member` by a member who is not the group's admin | Do not retry |
| `not_group` | `add_members`, `remove_member` or `leave` of a direct chat (or a stream's live chat) | Do not retry |
| `too_many_members` | The group would hold more than 100 members | Remove members first |
| `room_limit` | `open_direct` or `create_group` of a new room while you are listed in 1000 rooms | Leave some rooms first |
| `gone` | `create_group` under the `id` of a group everyone left, which holds messages | Create it under a new `id` |
| `bad_call` | A `call_decline`, `call_cancel`, `call_end`, `call_leave` or `call_expel` whose `call` is not a canonical lowercase UUID | Fix the client |
| `not_callable`, `call_failed`, `calls_disabled`, `no_call`, `ring_limited`, `expelled`, `call_full` | A call was refused; see [calls.md](calls.md#errors) | As there |
| `not_member` | The room has a member list without you; also sent unasked when you are removed from a room you are in, which you then no longer receive. For a member-list command: you are not on the room's list | Do not retry |
| `not_live` | A `stream` join of a stream whose chat the server has not opened | Retry once the stream is on air |
| `too_large` | A live chat message's `body` is over 2000 bytes | Send a shorter message |
| `not_joined` | `send` or `history` for a room this connection has not joined | Join first |
| `too_many_rooms` | This connection already holds 64 rooms | Use another connection, or leave some rooms by reconnecting |
| `rate_limited` | Past the send allowance, or the member-list allowance; `retry_after_ms` says when one more is allowed | Wait that long; the message was neither sequenced nor delivered, the list not changed |
| `busy` | Join or history allowance exceeded, too many sends awaiting answers, the room's owner queue is full, or too much unread output for a history page | Back off and retry |
| `unavailable` | The room's owner or the store could not be reached, or the server could not take the command just then; also sent unasked, with `room`, when the server could not confirm your membership of a room you are in (below), which you then no longer receive | Retry; resend a `send` with the same `id`; `join` a room it was sent unasked for again |
| `fenced` | The room changed owners while the write was in flight | Retry with the same `id` |
| `conflict` | This `id` was already used for a different message in the room | Send it under a new `id` |

## Limits

<!-- apps/chat/src/chat.hpp (Limits), codec/ws/include/codec/ws/decoder.hpp, apps/chat/src/session.cpp -->

| Limit | Value | On breach |
|---|---|---|
| Message size | 64 KiB per message (after reassembly) | Close `1009` |
| Binary frames | Not accepted | Close `1003` |
| Control frames | 8 per read; token bucket of 20, refilling 10/s | Close `1008` |
| Connections per address | 20 open at once from one address (IPv6: one /64) connecting directly, and 80 from all the /64s of one IPv6 /48 together; 10 new ones a second, 10 saved | Connection reset before the upgrade is read; back off and reconnect |
| Sockets per user | 16 open at once on a node | Upgrade answered `429`, `Retry-After: 5` |
| Rooms per connection | 64 | `error` `too_many_rooms` |
| New-room joins per user | Burst 64, then 1/s, across all the user's connections on a node | `error` `busy` |
| Sends | Burst 10, then 2/s, per user across the user's connections on a node | `error` `rate_limited` with `retry_after_ms` |
| Sends awaiting an answer | 128 KiB per connection, each counted as body + 256 bytes | `error` `busy` |
| Resends recognised | For about a minute on the node that sequenced or delivered them; always, by the store, once sequenced | |
| Resume and history output | 128 KiB queued behind a connection's unread output | Shorter page, or `busy` |
| History pages and resumes | Counted with joins: burst 64, then 1/s per user | `error` `busy` |
| Member-list changes | Burst 20, then 1 each 3 s per user across the user's connections on a node | `error` `rate_limited` with `retry_after_ms` |
| `rooms` and `members` pages | Counted with joins; 1 to 100 entries each | `error` `busy` |
| Group size | 100 members; 50 users named per `create_group` or `add_members` | `error` `too_many_members`; `malformed` |
| Lossy delivery | Nothing new while more than 64 KiB behind; then the newest 64 missed, oldest first | Gap in seqs; fill from history |
| Live chat message | 2000 bytes of body | `error` `too_large` |
| Live chat sends | Burst 40, then 20/s per chat per node, from all senders, on top of each user's | `error` `rate_limited` with `retry_after_ms` |
| Live chat history | The newest 1000 messages | Older ones are gone |
| Unread output | 256 KiB per connection | Connection closed; reconnect and rejoin |
| Handshake | 10 s from accept to a complete upgrade request | Connection closed |
| Idle | The server pings after 30 s of silence and closes after 75 s with nothing received | Answer pings (browsers do this themselves) |
| Server drain | Close `1001`, then 5 s | Reconnect |
| Token lifetime | The socket is closed when the token it was opened with expires: at its `exp` plus 60 s | Close `4001`: get a fresh token, reconnect, and `join` each room with `after` to resume |

## Presence

<!-- apps/chat/src/presence.hpp (PresenceLimits, IPresenceAccess), apps/chat/src/presence.cpp (watch, checked, recheck, on_member_removed), apps/chat/src/envelope.hpp, apps/chat/src/session.cpp (command), infra/postgres/src/message_sql.hpp (kSharedWith), docs/adr/0056-presence-over-the-room-plane.md, docs/adr/0096-member-lists-changed-by-their-users.md -->

A client can watch other users and hear when they come online and go offline. A user is online
while they have at least one open socket to any chat node, and for a grace of 10 s after their
last one closes: a page reload or a reconnect, even through another node, within the grace is
never reported. Nothing needs to be joined first.

**Only someone who shares a chat with the user may watch them** (ADR-0096): a direct or a group
chat both are listed in now. A stream's live chat lists nobody and shares nothing. The node the
watcher is connected to asks the database before it answers, so a `watch` of someone you share no
chat with is refused with `not_shared`, and nothing of that user's presence reaches you. When
someone is taken off a list (by a command, the service API or an operator), every node holds back,
at once, every watch involving them, watching or watched, and asks again; a watch that no longer
rests on a shared chat is dropped, and its connection is sent an unasked `error` `not_shared`
naming the user. A watch still shared carries on, with a `presence` for anything that changed
while it was held back. Adding someone to a chat does not start a watch: watch again.

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
| `error` | `reason`, `user` | The watch was refused (below), or, unasked, a watch was dropped: `not_shared` once you no longer share a chat with `user`. |

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
| `not_shared` | You share no direct or group chat with `user`; unasked, you no longer do | Do not retry until you share one |
| `unavailable` | The database could not say whether you share a chat | Watch again |
| `too_many_watches` | This connection already watches 128 users, those still being checked included | Unwatch some first |
| `busy` | This user asked the database about watches faster than 128 at once and then 2 a second (every watch not answered from memory counts, whether or not anyone on the node watches that user already, and unwatching and watching again counts each time), or, after the database let it in, this node watches as many users as it takes | Back off and retry |

Room ids of UUID version 8 (the third group starts with `8`) whose first byte is `02` (the id
starts with `02`) are reserved for presence: `join`, `send` or `history` naming one is refused
with `bad_room`. Presence events are never stored.

A `watch` costs one read of the database, by index, on the watcher's node; a removal costs one
read per connection on each node whose watches involve the user removed. A watch the database
refused is refused again from memory, without asking, until either user is listed in any chat
(or the node resyncs). When the database cannot answer about a watch already let in (after a
removal, or a resync), the watch stays held, telling nothing, and is asked again after 1 s, 2 s,
4 s and so on up to every 30 s, until the database answers: it is never let through on doubt, and
never dropped for an outage. After a resync, every watch on the node is held at once and asked
about 32 connections at a time, every 100 ms. Watching the same user
again while the first is checked is answered once per `watch` (16 at most) when the check
comes back.

## The service API

<!-- apps/chat/src/service_api.hpp, apps/chat/src/service_api.cpp, apps/chat/src/config.cpp (ULW_SERVICE_PORT), infra/auth/src/service_claim.cpp (read_service_claim, claim_holds), infra/auth/src/claims.cpp (is_service), docs/adr/0096-member-lists-changed-by-their-users.md -->

The embedding product's backend lists and unlists people itself, after its own request and
accept, rather than letting users do it (`ULW_CHAT_SELF_SERVICE=off`, the default). Chat serves it
plain HTTP/1.1 on a port of its own, `ULW_SERVICE_PORT`, on every node: any node will do, and
every node tells the users concerned of a change, as for their own commands. The port is not
routed from the internet; reach it from the backend's network only
([operator-contract.md](operator-contract.md), RUNBOOK step 10).

**Authentication.** `Authorization: Bearer <token>`, a token from the same identity provider as
users', verified the same way ([auth.md](auth.md)): signature, `iss`, `aud`, `exp`. It must also
be the service's by `ULW_SERVICE_CLAIM` and `ULW_SERVICE_SCOPE` (its `scope` holding
`ulw:admin`, say), issued to the client `ULW_SERVICE_CLIENT_ID` names (in `azp` or `client_id`),
which the backend gets from the provider's client-credentials grant
([auth.md](auth.md#service-tokens)). No cookie is read, and there is no shared secret. A missing
or invalid token is `401` with `WWW-Authenticate: Bearer`; a valid token that is not the
service's (a user's) is `403`; a key set that cannot be fetched is `503`.

**Requests.** `POST /service/v1/<operation>` with a JSON object body, at most 16 KiB; unknown
fields are refused. Answers are JSON, `Content-Type: application/json`. Connections are kept
alive; requests on one connection are answered in order, one at a time.

| Operation | Body | Answer (`200`) |
|---|---|---|
| `open_direct` | `users`: exactly two user ids | `{"type":"direct","room":..,"users":[as given],"added":[who it listed]}`. The pair's direct chat, the room `open_direct` names for either of them; the first time it lists both, after that it changes nothing (`added` empty). |
| `create_group` | `creator`, `id` (a request id: 1 to 64 of `A-Z a-z 0-9 _ -`), optional `users` (at most 50) | `{"type":"group","room":..,"id":..,"added":[...]}`. The group the creator's own `create_group` with that `id` names, the creator its admin; repeated with the same `id`, the same room, listing nobody more. |
| `add_members` | `room`, `users` (1 to 50) | `{"type":"added","room":..,"users":[who it listed]}`. As the group's admin would, without being on the list; those listed already are left as they are. Only into a group someone created: the `room` must be a group's id (`04`, else `bad_room`), the group must list someone (`404` `no_room` when nothing recorded it or it lists nobody), and one whose members all left over its history is `409` `gone`. |
| `remove_member` | `room`, `user` | `{"type":"removed","room":..,"user":..,"removed":[the user, or nobody]}`, with `"promoted":<user>` when the user was the group's last admin and the member whose id sorts first became admin, as for a `leave`. The user's sockets are out of the room at once, on every node. Idempotent: a user not listed (any more), or a room nothing recorded, answers `200` with `"removed":[]`. |
| `close_direct` | `room` (a direct chat's id, `03`, else `bad_room`) | `{"type":"closed","room":..,"removed":[who it unlisted]}`. Takes a direct chat apart (an unfriend, a block): both of the pair unlisted under the room's lock, so their sockets leave the room at once on every node and their watches of each other are dropped (`not_shared`) unless they share another chat. Idempotent: `"removed":[]` when it lists nobody. The history stays; a later `open_direct` lists the pair again in the same room, history included. |
| `rooms` | `user`, optional `after` (a room id), `limit` (1 to 100, default 50) | `{"type":"rooms","rooms":[{"room","kind","role","peer"}],"more":..}`, as the socket's `rooms` answers that user. |
| `members` | `room`, optional `after` (a user id), `limit` (1 to 100, default 50) | `{"type":"members","room":..,"members":[{"user","role"}],"more":..}` for any room; a room nobody is listed in answers no members. |

Changes go through exactly the database statements users' commands do, under the room's lock:
the same kinds (a direct chat's pair never changes), the 100-member cap, the 1000-room cap on
`open_direct`'s first user and `create_group`'s creator, the same history rule for a reused group
`id`, and the same `member` frames to every socket concerned. Calls and joins still need
membership; the API changes who is listed, nothing else.

```http
POST /service/v1/open_direct HTTP/1.1
Host: chat-service.ulw.svc
Authorization: Bearer eyJhbGciOi...
Content-Type: application/json
Content-Length: 30

{"users":["user-1","user-42"]}
```

```http
HTTP/1.1 200 OK
Content-Type: application/json

{"type":"direct","room":"03db1b2d-a8da-8387-a668-3abc70b6953e","users":["user-1","user-42"],"added":["user-1","user-42"]}
```

Errors are `{"type":"error","reason":..}`:

| Status | `reason` | When |
|---|---|---|
| `400` | `not_json`, `malformed`, `bad_room`, `bad_user`, `bad_id`, `self` | The body is not one of the operation's: not JSON, a missing, unknown or mistyped field, a list too long, a room that is not a canonical lowercase UUID (or is a presence room), a user or request id out of form, `open_direct` naming one user twice |
| `401` | `unauthorized` | No bearer token, or one that fails verification |
| `403` | `forbidden` | A valid token that is not the service's: without the service claim, or issued to another client than `ULW_SERVICE_CLIENT_ID` |
| `404` | `not_found`, `no_room` | Another path; `add_members` of a group nothing recorded or that lists nobody |
| `405` | `method_not_allowed` | Not `POST` (`Allow: POST`) |
| `409` | `not_group`, `too_many_members`, `room_limit`, `gone`, `not_member` | As the socket's commands: a direct chat's pair, the 100-member cap, the creator or first user in 1000 rooms, a group `id` whose group everyone left and that holds messages, a direct chat that lists others than the pair or a repeated create whose creator left |
| `413` | `too_large` | A body over 16 KiB |
| `429` | `rate_limited`, with `retry_after_ms` and `Retry-After` | Past the service's rate, or past the failures' budget of the caller's address (below) |
| `503` | `unavailable`, with `Retry-After: 1` | The database or the key set could not be reached; retry, the same request again (changes are idempotent) |

A refusal at the head (`404`, `405`, `413`, a request that is not HTTP) and a `429` for spent
failures close the connection after the answer; the others keep it.

**Limits.** Per node:

- **The service's rate:** 100 requests at once, then 50 a second, taken only by requests whose
  token is the service's, once it is verified: nobody else can spend it. Three nodes take 150
  changes a second in all, each a transaction holding one room's row for milliseconds, far past
  a product's sign-ups and invitations.
- **Failures, per caller address:** every request is charged one before its token is verified
  and given it back once the token is the service's; 20 at once, then 5 a second. Past it, every
  request from that address is answered `429` before its token is looked at, so a stranger can
  neither verify tokens without end nor touch the backend's rate.
- **Connections:** 32 open at once, at most 8 from one address (more are closed at accept). A
  connection must send its first byte within 5 s, then a whole request within 10 s of its first
  byte; an idle one is closed after 30 s.

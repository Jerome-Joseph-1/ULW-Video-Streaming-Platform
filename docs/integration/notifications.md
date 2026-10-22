# Notifications

<!-- apps/chat/src/notify.hpp, apps/chat/src/notify.cpp (Notifier), apps/chat/src/notify_sender.cpp (HookSender, notify_signature), docs/adr/0130-a-hook-batch-says-what-was-lost.md, apps/chat/src/ring.cpp (Ringer::observe, Ringer::announce, Ringer::ringing), apps/chat/src/session.cpp (device_of, Session::set_visible), apps/chat/src/notify.cpp (Notifier::message_text), docs/adr/0126-a-message-names-who-is-present.md, apps/chat/src/service_api.cpp (decline_call, call_status), apps/chat/src/config.cpp (notify_config), infra/postgres/src/notify_store.cpp, migrations/0022_notify.sql, apps/chat/src/video_relay.cpp (VideoRelay), migrations/0023_video_events.sql, docs/adr/0111-notification-events-for-members-not-connected.md, docs/adr/0120-video-events-on-the-notification-hook.md, apps/chat/src/notify.cpp (Notifier::sequenced), apps/chat/src/chat.cpp (RoomLog::on_sequenced), rt/src/room_router.cpp (fan_out), docs/adr/0133-the-backend-reads-and-hears-every-message.md -->

Chat reaches a member only through the sockets they have open ([chat.md](chat.md),
[calls.md](calls.md)). For a member with none (a closed tab, a phone in a pocket), the operator
can have chat tell its own backend what happened, as **events** POSTed to a URL inside the
cluster. The backend decides whether to notify and how: its Web Push subscriptions, its VAPID
keys and its notification text. Chat never sends a push itself, and an event never carries a
message's content.

**Off by default.** With `ULW_NOTIFY_URL` unset, chat sends nothing, writes nothing to the
registry below, and behaves exactly as it does without this page. Nothing here changes what a
socket hears.

## Turning it on

| Variable | Value |
|---|---|
| `ULW_NOTIFY_URL` | The receiver: `http://` or `https://`, inside the cluster (a Service), e.g. `http://push-receiver.askedin.svc.cluster.local:8080/ulw/events` |
| `ULW_NOTIFY_SECRET` | At least 32 bytes; the key every request is signed with. Required with the URL. |
| `ULW_NOTIFY_MESSAGE_INTERVAL_SECONDS` | 5 to 3600, default 30: how often one member may be told of one room's messages while they stay away |
| `ULW_NOTIFY_MESSAGE_PRESENT` | `on` or `off` (the default): `message` also names the members with a socket in view, and their devices ([`message`](#message)) |
| `ULW_NOTIFY_SEQUENCED` | `on` or `off` (the default): every message in a direct or group chat is also a [`message_sequenced`](#message_sequenced) |

On Kubernetes, `components/notifications` sets all three from `config.env` (`NOTIFY_URL`,
`NOTIFY_NAMESPACE`, `NOTIFY_PORT`, `NOTIFY_MESSAGE_INTERVAL_SECONDS`) and `NOTIFY_SECRET` in chat's
Secret, and lets chat reach the receiver's port (RUNBOOK step 7, "Notifications"). Run the
receiver behind a NetworkPolicy that admits chat alone: the events name users and devices.

Every chat node sends events: a call's from the room's owner, a message's from the sender's node,
and `message_sequenced` from the room's owner. The receiver sees one stream per node and merges
them.

## Devices

A socket names the device it belongs to in its upgrade, `wss://<CHAT_HOST>/rt?device=<uuid>`:
the same id the client sends in `call` ([calls.md](calls.md#the-flow)) and that the backend links
its push subscription to. It is optional; a value that is not a UUID, or a second `device`, is
refused `400`. Chat records, for the hook only, which devices of which users have a socket open
on which node. A socket that names no device still counts as its user's for messages; it cannot be
listed in a call's `devices`.

Name the device on every socket, so that `call_ringing` can say which devices already ring: the
backend then pushes to the others only.

A socket counts only while its page or app is in view. A socket that says it is hidden
(`{"type":"state","visible":false}`, [chat.md](chat.md#messages)) is left out of the registry
until it says it is back: `call_ringing` does not list its device, `message` counts its user as
away if it was their only one, and `presence` leaves it out (ADR-0125). A phone that is locked
or backgrounded with its socket still open then gets the push it would otherwise miss.

## Delivery

```http
POST /ulw/events HTTP/1.1
Content-Type: application/json
ULW-Signature: t=1767225600,v1=9ad04090ab1beebfe548dca4c8329f79a53d98be77244b3baea55fe1de9d5412

{"run":"9f3c2a1be04d7765","seq":118,"lost":0,"events":[{"id":"...","type":"call_ringing",...},{"id":"...","type":"call_answered",...}]}
```

- **Batches.** Each request carries 1 to 64 events, oldest first, as `{"events":[...]}`. One
  request at a time per node, so a node's events arrive in the order it made them.
- **What was lost** (ADR-0130). `run` names one run of one node (a new one at every start).
  `seq` counts that run's requests from 1, and `lost` is how many events the run dropped since
  the request before this one went out: a full queue's, and a failed request's. A `seq` that
  skips one, for the same `run`, is a request that never arrived. Either way the events are not
  sent again: reconcile from your own records (`call_status`, the service listings), or show
  the user a gap.
- **Signature.** `ULW-Signature: t=<unix seconds>,v1=<hex>`, the hex being HMAC-SHA256 under
  `ULW_NOTIFY_SECRET` of `<t>.<body>`, the body exactly as received. Check it with a
  constant-time compare, and refuse a `t` more than a few minutes from your clock.
- **Answer.** Any `2xx`, within 2 s. Anything else, or no answer within 2 s, and that request's
  events are dropped: they are **never sent again**.
- **At most once, best effort.** Each node queues at most 4096 events; past that the newest is
  dropped. A receiver that is down, slow or unreachable loses events, which each node counts
  (`notify_events_total{outcome="dropped"}` and `{outcome="failed"}`) and the next request says
  (`lost`). A ring, a ticket and a
  message never wait on the receiver.
- **`id`.** Every event has a UUID; drop one you have seen.
- **`at`.** Unix seconds when the node made the event.

Answer at once and do the pushing after: a receiver that takes its 2 s for each batch holds up
every event behind it on that node.

## Events

### `call_ringing`

From the room's owner, once per call, when it starts ringing (not when the ring is announced
again to sockets every 15 s).

```json
{"id":"0199e7b2-5b0c-7a51-8f4e-1c2d3e4f5a6b","type":"call_ringing","at":1767225600,
 "room":"03db1b2d-a8da-8387-a668-3abc70b6953e","call":"0199e7b2-5b0b-7c11-9a2b-3c4d5e6f7a8b",
 "kind":"direct","caller":"user-1","expires_at":1767225645,
 "callees":[{"user":"user-42","connected":true,"devices":["0199e7b2-1111-7aaa-8bbb-cccccccccccc"]}]}
```

| Field | |
|---|---|
| `room`, `call` | The room, and the call's id, which every later event of the call, the client's `answer` and the service API name |
| `kind` | `direct` or `group` |
| `caller` | Who called |
| `expires_at` | Unix seconds: the ring stops then if nobody answers. Expire the notification then even if no other event comes (below) |
| `callees` | Every member it rings (a group call: up to 32), each with `connected`, whether they have any socket open on any node, and `devices`, those of their devices with a socket open that named themselves. Both `null` when the registry did not answer within 1 s: unknown, push to all |

`devices` is a hint, as fresh as the last write (each node writes its sockets' changes a quarter
of a second after them): a device whose socket closed a moment ago may still be listed, and a
node that died is counted for up to 60 s. Spare the devices listed; push to the others.

### How a ring ends

From the room's owner, once each, in order after the call's `call_ringing`:

```json
{"id":"...","type":"call_answered","at":1767225604,"room":"...","call":"...","kind":"direct","caller":"user-1","by":"user-42"}
```

| `type` | `by` | When |
|---|---|---|
| `call_answered` | who answered | A callee's ticket answered it (a group call: each member who comes) |
| `call_declined` | who declined | A callee declined, from a socket or through `decline_call` (a group call: each one); with `reason` and `text` when they gave them (ADR-0115) |
| `call_cancelled` | the caller, or `null` | The caller gave up before anyone answered. `null` with `"reason":"caller_left"`: the caller was no longer in the call when it rang (ADR-0113). `null` with `"reason":"moved"`: the room changed owner while it rang (ADR-0129) |
| `call_missed` | `null` | Nobody answered by `expires_at`; `users` names who missed it. A group call that goes on without someone: `users` is those still rung when it rang out |
| `call_ended` | who ended it, or `null` | The call was over after it was answered, or after a group call's ring ran out with only its caller in it (a member ended it, the last one left, or nobody was in it) |

Each has `room`, `call`, `kind` and `caller` as above. Withdraw the notification of the call on
any of them (for a group call, `call_answered` and `call_declined` withdraw only `by`'s). A
group call's members leaving and being put out are not events.

**A ring whose room changes owner.** When the room's owner changes while a call rings (a
deploy, a node failure), `call_cancelled` with `"reason":"moved"` follows its `call_ringing`
(ADR-0129). It comes from the old owner as it drains, or from the new owner as it takes the
room. A node that dies after its last batch, before any new owner takes the room, sends nothing,
so still expire every call notification at its `expires_at`, and ask `call_status` before
showing one that arrives late. With `room` alone, `call_status` answers the
room's call whatever its id, and who is in it ([calls.md](calls.md#after-a-reload)).

### `message`

From the node the sender is on, once the message is sequenced, for a direct or group chat (never
a stream's live chat):

```json
{"id":"...","type":"message","at":1767225700,"room":"...","sender":"user-1","seq":118,"users":["user-42"]}
```

`users` are the room's members who are owed word of it: listed in the room, not the sender, with
no socket open on any node, and not told of this room within the interval
(`ULW_NOTIFY_MESSAGE_INTERVAL_SECONDS`). A member told is not told of that room again until the
interval passes or they connect, whichever is first; a busy room makes at most one event per
member per interval, however many messages it carries. A message that owes nobody makes no
event. Never the body: fetch what to show through your own backend, or show "New message from …".

With `ULW_NOTIFY_MESSAGE_PRESENT=on` (ADR-0126), the event also says who is **present**: the
members, not told of this room within the interval, who have a socket in view somewhere, each
with the devices those sockets named (a socket that named none adds none):

```json
{"id":"...","type":"message","at":1767225700,"room":"...","sender":"user-1","seq":118,"users":["user-42"],"present":[{"user":"user-7","devices":["01a0eb86-..."]}]}
```

Push to a present member's other devices: someone reading on a laptop still gets the push on
their phone. A present member counts as told, as `users` do, so read `present` once you turn
this on: a receiver that ignores it misses those members for the interval. An event goes out
when either list has anyone; without the option, `present` is never there and nothing else
changes. Chat's Deployment takes the variable with a patch in the overlay; the notifications
component does not set it.

### `message_sequenced`

With `ULW_NOTIFY_SEQUENCED=on` (ADR-0133), from the room's owner, once for every message it
sequences in a direct or group chat, quiet ones included, whoever is owed word of it:

```json
{"id":"...","type":"message_sequenced","at":1767225700,"room":"...","seq":118,"sender":"user-1","message_id":"m-7f3a","sent_at":1767225700123,"quiet":false}
```

| Field | |
|---|---|
| `room`, `seq`, `sender` | The message, as every socket hears it |
| `message_id` | The `id` the sender sent it with ([chat.md](chat.md#messages)) |
| `sent_at` | When the owner sequenced it, Unix milliseconds: the message's `at` on the socket |
| `quiet` | Its sender sent it quiet (a reaction, an edit) |

What a backend checks a claim against ("seq 118 of this room is user-1's") without reading the
database. It is not throttled, not looked up and never carries the body. A stream's live chat
makes none. Like every event it is at most once: a lost batch, or an owner that died between
storing a message and telling it, leaves a gap in `seq`. Fill it with the service API's
`history` and `after` ([chat.md](chat.md#the-service-api)), which also gives the bodies. It costs
one event per message and nothing more.

### `video_ready` and `video_failed`

When one of your users' videos finishes processing, or fails (ADR-0120). Sent by one chat node,
whichever takes the change first, up to 2 s after it:

```json
{"id":"...","type":"video_ready","at":1767225800,"video":"0199950c-...","owner":"auth0|123","version":2,"duration_ms":6000}
{"id":"...","type":"video_failed","at":1767225800,"video":"0199950d-...","owner":"auth0|123","version":3,"error_reason":"no video stream"}
```

`at` is when the video changed. `version` is the video object's, so an event older than what you
hold can be ignored. A video deleted before it settled makes no event. A change no node takes
within an hour (the hook off, or every node down) is forgotten. Like every event, these are at
most once: keep the [service listing](videos-and-playback.md#service-api-visibility-takedown-and-listing)
as the source of truth, and poll it now and then for any you missed.

### Asking who is connected

The service API's `presence` ([chat.md](chat.md#the-service-api)) reads the same registry: for
up to 64 users, whether each has a socket open on any node and which devices those sockets named.
Use it before a push you would rather skip for someone who can see the app.

## Answering and declining from a notification

- **Answer.** The notification opens the app, which connects (`/rt?device=<uuid>`), `join`s the
  room and asks for a ticket naming the ring, `{"type":"call","room":..,"device":..,"answer":<call>}`
  ([calls.md](calls.md#ringing)). A socket opened after the ring started answers it like any other.
  If the call ended meanwhile, the answer is `no_call`.
- **Decline**, without opening the app: the backend asks chat's service API
  ([chat.md](chat.md#the-service-api)) on the user's behalf.
- **Is it still ringing?** Before showing a late `call_ringing`, the backend asks `call_status`.

| Operation | Body | Answer (`200`) |
|---|---|---|
| `decline_call` | `room`, `call`, `user`, optional `reason` and `text` | `{"type":"call_declined","room":..,"call":..,"by":<user>}`. As the user's own `call_decline`: checked against the room's member list on its owner, and only a callee it still rings may. Every socket of the call hears `call_declined`, and the hook gets it too. `409` `no_call` when the call no longer rings for them (answered, over, or never theirs), `409` `not_member` when the user is not on the list |
| `cancel_call`, `end_call`, `leave_call` | `room`, `call`, `user` | As `decline_call`, for the user's own `call_cancel`, `call_end` or `call_leave`: what a page that is going away reaches your backend to say (ADR-0114). Answered with the event everyone hears (`call_cancelled`, `call_ended`, `call_left`) |
| `call_status` | `room`, `call`, optional `user` | `{"type":"call_status","room":..,"call":..,"status":"ringing"\|"answered"\|"over"}`. `ringing`: it still rings (for `user`, when named: a group call may be on while it still rings for some); `answered`: it is on, and no longer rings (for them); `over`: the room has no such call now, or `user` is not in it |

Both are asked of the room's owner from whichever node takes the request, as a socket's command
would be; `503` `unavailable` when the owner could not be reached (retry). Rooms are given as for
the other operations; a stream's live chat is `400` `bad_room`.

## What it costs

With the hook on, each socket open and close is a row write in Postgres, batched a quarter of a
second; each node renews its lease every 20 s; a ring is one more read on its owner; and a
message in a direct or group chat is one more statement on its sender's node. Each node also
takes the videos' changes, one statement every 2 s. With it off, none of that; the videos'
changes are still recorded and forgotten an hour on.

# 0056. Presence over the room plane, with a grace and a lease

Status: Accepted, amended by ADR-0096
Date: 2026-09-29

## Context

Section 8.15 asks for presence built as "a room whose members are the watchers, using the same
registry and forwarding", with a flap guard (a disconnect starts a grace timer, and offline fans
out only if the user has not reconnected when it fires), and with a user nobody watches costing
no messages. M18 is done when a reconnect within the grace produces no event, a user who does
not come back produces exactly one offline event on every node that watches them, and a user
with no watchers produces no messages, shown by a counter.

The room plane (ADR-0015, ADR-0035) gives each room one owner that sequences every write and
fans it out, once per seq, to every node subscribed at that moment. ADR-0043 froze the layering
for Phase 3: `apps/chat/src/chat_service.cpp` and `rt/src/room_router.cpp` must not change for
end-to-end encryption, and commands of other families are dispatched by the session to services
of their own. A user can be connected through several nodes at once, and a node can die without
a word.

## Options

| Question | Option | Verdict |
|---|---|---|
| Where presence lives | Inside `ChatService` | Rejected: it would grow the file M22 freezes, for something that is not chat |
| | A service of its own beside it, reached from the session, talking to the same `IRooms` | Accepted |
| | Owner-side logic in the router: the owner knows which nodes subscribe | Rejected: `room_router.cpp` would learn what a presence room is, and only the owner could decide, so every node's connects and disconnects would be forwarded to it |
| Who decides offline | The owner of the user's presence room | Rejected, as above |
| | Each watching node, from the room's order | Accepted: every node in the room sees the same events in the same order, so each reaches the same verdict once, with no coordinator but the order the plane already gives |
| Where the grace runs | At the watchers, after an offline sent at once | Rejected: every flap would cost two messages to every watching node |
| | At the user's node, before anything is sent | Accepted: a flap costs nothing at all |
| How the user's node learns it is watched | Always announce online and offline | Rejected: the user nobody watches pays for it |
| | A directory of who watches whom | Rejected: a second store and a second consistency problem |
| | Join the user's room while they are connected and listen: watchers say hello, and a join finding the room's head above 0 probes for watchers that spoke before | Accepted: an unwatched user's room was never written to, its head is 0, and nothing is sent |
| How a dead node's announcement goes away | Watch node-channel links | Rejected: a node only has a link to owners it routes through, which says nothing about the user's node |
| | A lease: announcements are renewed while another node watches, and forgotten when not | Accepted: costs one message a minute per watched online user, and nothing for anyone else |

## Decision

- **Rooms.** A user's presence room is the RFC 9562 version 8 UUID of SHA-256 over a fixed
  namespace and the user id (`presence_room.hpp`): every node derives it, nothing is looked up.
  The registry, ownership, fencing and forwarding are the chat rooms' own.
  Events are 9 bytes: a kind and the sender's tag, 64 bits of SHA-256 over the node's name and
  its start time, so a restarted node is a new sender and its last run's announcements run out
  on their own.
- **Kind tag.** Rooms derived from a name share version 8, so the first byte of the id says
  what names the room: `core::ports::NamedRoom` has `StreamChat = 0x01`, a stream's live chat
  (M32, `core::ports::is_stream_chat`), and `Presence = 0x02`, a user's presence room.
  `presence_room()` overwrites the digest's first byte with `NamedRoom::Presence` and leaves
  the RFC's version and variant bits as they are, the rest of the digest filling the other
  bits, so a presence room and a stream's chat can never share an id however their names
  collide. The enum, `is_named_room()` and `is_stream_chat()` are in
  `core/include/core/ports/message_store.hpp` with the names and values M32 gives them, so M32
  merges onto them unchanged. A unit test pins alice's room id and its tag.
- **Ephemeral.** A version 8 id tagged `0x02` is the room plane's ephemeral kind
  (`rt::is_ephemeral_room`); other version 8 ids, a stream's chat among them, are ordinary
  rooms whose messages are kept. The store creates an ephemeral room with kind `presence` in
  `room_state` and takes its seqs with the fenced `UPDATE room_state SET last_seq = last_seq +
  1` alone, storing no `chat_messages` row, in Postgres and in the in-memory stores alike (a
  conformance law). The ephemeral rule wins over any kind recorded in `chat_rooms`; a presence
  room is not a chat `RoomKind`, since its joins come from nodes through `IRooms` and never
  pass the message store's `admits()`. The envelope refuses presence room ids in `join`, `send`
  and `history` (`bad_room`), so no client can read or write a presence room, and only nodes
  speak in one.
- **Created without a join.** A presence room is the one room the room plane creates with no
  chat join before it, so nothing has recorded it in `chat_rooms`. Its creation records it
  there itself, as a closed room (`group_chat`), in the statement that creates it, as M19's
  joins record a room before its member row (ADR-0054); every room created with no kind
  recorded is. The server's `record_live` then conflicts on the `chat_rooms` key: one that runs
  after the creation, or waits on it, finds the room closed and is refused, and a creation that
  runs while an uncommitted `record_live` holds the key waits for it and takes its kind. Reading
  `chat_rooms` instead, as the creation did before, missed an uncommitted `record_live`, created
  the room `group_chat` in `room_state`, and left `chat_rooms` saying live once it committed. A
  Postgres test holds a `record_live` open while the room plane creates the room.
- **Membership.** A node joins the room while the user has a connection there or is in their
  grace, or while a client there watches them; the node is one member for all of those, as
  `ChatService` is for a chat room. It leaves when none of that holds and nothing it said is
  outstanding.
- **Events.** `hello` (a node started watching), `unwatch`, `probe` (online, and asks watching
  nodes to `ack`: a first announcement in a room something was said in, and every renewal),
  `ack`, `online` (an answer to a hello), `offline`. Each is
  idempotent, so a send that failed (its fate unknown after `unavailable`) is sent again, a
  second later, if the state that called for it still holds. A node has one event in flight per
  room, so its events keep the order it decided them in.
- **Verdict.** At a watching node the user is online while any node's `online` or `probe` stands
  (not followed by that node's `offline`), or while they are connected there or in their grace.
  Clients hear `watching` with the state when they watch, then `presence` on each change of it.
  One seq reaches a node once (the router's per-room delivered check), so each change is one
  event per watching node.
- **Flap guard.** The last connection closing starts a 10 s grace (`PresenceLimits::grace`,
  derived there; `ULW_PRESENCE_GRACE_MS` overrides it). A connection within it cancels it and
  nothing is sent. When it runs out the node sends `offline` if it had announced the user, and
  nothing if it had not. A reconnect through another node within the grace is that node's
  `probe` followed by the first node's `offline`: the watchers' verdict stays online throughout.
- **Zero subscribers.** A user nobody watches has a room joined on their node, whose head is 0,
  and no event is ever sent. The head a join answers is the room's `last_seq`, also after the
  room changed owners: a node taking a room over reads `last_seq` in the statement that claims
  it (`Ownership::last_seq`, `RoomRegistry::taken_at`) and counts on from there, so a hello said
  under an earlier owner still shows. The join is membership, not a message: it costs a registry
  lookup (a claim, the first time) and a place in the owner's heartbeat, both already bounded by
  `Limits::max_connections`. `presence_events_sent_total` counts every event, and the cluster
  test checks it, with `forwards_total`, does not move for such a user.
- **Timers.** One reactor timer per node: at once for rooms with something to send, otherwise at
  the next scan. A scan each second visits every room for graces, renewals and expiries, which
  are each at most a second late. Time is the injected clock's.
- **Node failure.** Who is owed an event is the owner's view of the room's members at the moment
  it sequences it: a node subscribed then gets it once, a node that joins later asks with
  `hello`.
  - The user's node dying, or draining before a grace runs out, leaves its announcement
    standing. Nodes watching drop it once it has not been renewed for 150 s (a `probe` every
    60 s while another node watches; the derivation is beside `PresenceLimits`) and report
    offline then, once.
  - A watching node that dies leaves its tag in the announcing node's list of watchers. The
    renewal probes it no longer acks let that entry expire after the same 150 s, and renewals
    stop once no other node is left.
  - The owner dying mid fan-out can cost some nodes an event. A node sees that as a jump in the
    room's seqs at its next delivery and says again what it stands for: a `hello` if it watches,
    a `probe` if the user is connected there (a probe, so that acks lost in the gap are sent
    again and the node does not expire a watcher that is still there). A lost `offline` needs no
    repeat, since the lease drops the announcement anyway. The router's join answer can itself
    lag a seq the old owner took and never delivered; the same jump shows it.
  - A watching node whose copy of an announcement expires while clients there still watch says
    `hello` again: if only the renewals were lost, the announcing node answers, and the user is
    shown online again after one offline.
- **Limits.** 128 watches per connection, `too_many_watches` past it; watching oneself is
  `watching_self`. 8192 presence rooms per node (half the router's 16384): a watch that would
  make a new room past it is `busy`. Every watch that makes this node start watching a user
  (the room's local watchers go from none to one, which costs a `hello` and later an `unwatch`)
  takes a token from the watching user's bucket of 128 refilling at 1 a second, `busy` past it,
  so watching and unwatching in a loop is held to a second per round. At most 64 nodes are
  remembered per room. About 1 KiB per room (8 MiB) and 16 bytes per watch (a room pointer in
  the connection's list, an id in the room's; 2.6 MiB for 1280 connections of 128): 11 MiB of
  the node's budget.
- **Layering.** `chat_service.cpp` is untouched: the session dispatches `watch` and `unwatch`
  to `Presence`, which uses `IRooms` as `ChatService` does. `room_router.cpp` changed in one
  place, its `head()`: a node taking a room over now counts on from the `last_seq` its claim
  read instead of from 0. That is a fix to the room plane, which every room needed (a member
  joining after a takeover was told a head below what had been said), not something presence
  asks of it. M22's freeze covers the E2EE phase only, the phase-2 tag to the phase-3 tag
  (`tools/e2ee_diagnostic_check.sh`), so this change, made before phase-2, is outside it, and
  later phases may change the file again. Presence events are the nodes' own bytes in rooms no
  client can reach; end-to-end encryption, which is about chat bodies, has nothing to do with
  them.

## Consequences

- Every connected user costs their node one joined room, whether or not anyone watches. That is
  the price of hearing a hello without a directory; rooms are bounded by connections (1280),
  well inside the router's cap.
- A user who was ever watched costs a `probe` and an `offline` per session from then on, watched
  or not: the room's `last_seq` is above 0 for good, and the user's node cannot tell a room
  whose watchers left from one with watchers who spoke before it joined.
- A node that dies or drains holds its users online for up to 150 s after it stops renewing, and
  a watching node that lost an `online` in an owner's failure shows the user offline until the
  room's next event shows the gap, a minute at most (the next renewal). Both are bounded by the
  lease; neither shows a flap.
- A watch answered `watching` with `offline` for a user connected elsewhere is followed within a
  round trip by `presence` `online`: the answer is what the node knew, before the hello's
  answers arrived.
- Presence events are sequenced and fanned out but never stored: a room's history is not a
  record of who was online when. A client that missed events cannot fetch them; it watches
  again and is answered with the state.
- **Open before production: authorization.** Anyone signed in may watch anyone, and learn when
  they are online. Nothing in the brief asks for a rule, and the envelope has no place for one
  yet; whether watching needs a shared room, a contact, or a check with Askedin's platform must
  be decided before presence is offered to real users.

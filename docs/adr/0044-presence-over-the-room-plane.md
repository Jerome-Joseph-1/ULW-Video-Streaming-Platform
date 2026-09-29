# 0044. Presence over the room plane, with a grace and a lease

Status: Accepted
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
  The registry, ownership, fencing and forwarding are the chat rooms' own. The envelope refuses
  version 8 room ids in `join` and `send` (`bad_room`), so no client can read or write a
  presence room, and only nodes speak in one. Events are 9 bytes: a kind and the sender's tag,
  64 bits of SHA-256 over the node's name and its start time, so a restarted node is a new
  sender and its last run's announcements run out on their own.
- **Membership.** A node joins the room while the user has a connection there or is in their
  grace, or while a client there watches them; the node is one member for all of those, as
  `ChatService` is for a chat room. It leaves when none of that holds and nothing it said is
  outstanding.
- **Events.** `hello` (a node started watching), `unwatch`, `probe` (online, and asks watching
  nodes to `ack`), `ack`, `online` (an answer to a hello, or a renewal), `offline`. Each is
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
  and no event is ever sent. The join is membership, not a message: it costs a registry lookup
  (a claim, the first time) and a place in the owner's heartbeat, both already bounded by
  `Limits::max_connections`. `presence_events_sent_total` counts every event, and the cluster
  test checks it, with `forwards_total`, does not move for such a user.
- **Timers.** One reactor timer per node: at once for rooms with something to send, otherwise at
  the next scan. A scan each second visits every room for graces, renewals and expiries, which
  are each at most a second late. Time is the injected clock's.
- **Node failure.** Who is owed an event is the owner's view of the room's members at the moment
  it sequences it: a node subscribed then gets it once, a node that joins later asks with
  `hello`. A watching node that dies loses nothing anyone else needs, and its clients watch again
  when they reconnect. The user's node dying, or draining before a grace runs out, leaves its
  announcement standing: nodes watching drop it once it has not been renewed for 150 s (renewals
  every 60 s while another node watches; the derivation is beside `PresenceLimits`), and report
  offline then, once. The owner dying mid fan-out can lose an event for some nodes: a lost
  `offline` ends the same way, a lost `online` is repaired by the next renewal.
- **Limits.** 128 watches per connection, `too_many_watches` past it; 8192 presence rooms per
  node (half the router's 16384), and a watch that would join a new room past it, or past the
  user's bucket of 128 refilling at 1 a second, is answered `busy`. At most 64 nodes are
  remembered per room. About 1 KiB per room: 8 MiB of the node's budget.
- **Layering.** `chat_service.cpp` and `room_router.cpp` are untouched: the session dispatches
  `watch` and `unwatch` to `Presence`, which uses `IRooms` as `ChatService` does. Presence
  events are the nodes' own bytes in rooms no client can reach; end-to-end encryption, which is
  about chat bodies, has nothing to do with them.

## Consequences

- Every connected user costs their node one joined room, whether or not anyone watches. That is
  the price of hearing a hello without a directory; rooms are bounded by connections (1280),
  well inside the router's cap.
- A user watched once and no longer still costs a `probe` and an `offline` per session while the
  owner of their room that heard it keeps it: the head is above 0 until the room is given up
  (60 s after its last member left).
- A node that dies or drains holds its users online for up to 150 s after it stops renewing, and
  a watching node that lost an `online` in an owner's failure shows the user offline for up to a
  minute. Both are bounded by the lease; neither shows a flap.
- A watch answered `watching` with `offline` for a user connected elsewhere is followed within a
  round trip by `presence` `online`: the answer is what the node knew, before the hello's
  answers arrived.
- Once messages are stored (M19), every presence event is a row: the store keeps what it is
  asked to sequence. They are 9 bytes each and only watched users make them.
- Anyone may watch anyone, as anyone may join any chat room until M19's membership checks;
  whether presence follows those checks is M19's to decide.

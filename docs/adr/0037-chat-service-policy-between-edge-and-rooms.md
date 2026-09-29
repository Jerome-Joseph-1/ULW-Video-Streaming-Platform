# 0037. The chat service: policy between the client edge and the room plane

Status: Accepted
Date: 2026-09-29

## Context

ADR-0035 and ADR-0036 built the room plane and the client edge and left four things to the chat
service: rate limits, a client message id so that the retry after `unavailable` is not
delivered twice, acks and resume from a seq (with the gaps a room changing owners leaves), and
the final client envelope. Section 8.15 adds that the service owns policy (codecs never see it),
that bodies are opaque bytes end to end, never parsed, logged, indexed or moderated, that a
room has one total order by `last_seq`, that durable rooms buffer for briefly disconnected
clients and lossy rooms drop for slow consumers. Phase 3 then puts MLS on top, and its
checkpoint (M22) requires that `apps/chat/src/chat_service.cpp` and `rt/src/room_router.cpp`
need no change for it: whatever end-to-end encryption needs from them has to be there now.

## Options

| Question | Option | Verdict |
|---|---|---|
| Where a retry is recognised | At the client's node only, by the key of each `sent` answer | Rejected: after `unavailable` the node has no answer to remember; only the owner, which sequenced it, knows |
| | In the router: the key travels with the message to the owner and back out in every delivery; the owner checks it before appending, and every node checks it before forwarding | Accepted |
| | A unique index on (room, sender, key) in Postgres | Right once messages are stored (M19); in M17 nothing is |
| How the service reaches the room plane | Each session stays a room member, as in M16 | Rejected: one delivery per session per message crosses the node channel's fan-out, and there is nowhere to keep what a session missed |
| | The service joins each room once per node and hands every delivery to its clients | Accepted: one member per room, one encoded text per message, and one place to keep the room's latest messages |
| What a body is on the client wire | A JSON string, as in M16 | Rejected: ciphertext is not UTF-8, and a client wrapping it in base64 inside a string would store base64 as the "bytes", compressible and twice encoded |
| | Base64url in the JSON, decoded at the edge into bytes | Accepted: bytes from the edge on; the transfer encoding is not content |
| | Binary WebSocket frames with a header of our own | Rejected for now: a second framing next to the JSON one for the same commands, for a third of the size |
| Commits of a room ordered per epoch | The owner reads each body's MLS framing and refuses a second commit for an epoch | Rejected: the owner would parse bodies, which it must never do |
| | A generic "first write per slot" check in the owner | Rejected: an interface ahead of its caller, and a slot high-water mark that dies with the owner until M19 persists it |
| | The room's total order is the order: the first commit for an epoch, in seq order, is the one every member applies, and members discard later ones | Accepted: needs no reading of bodies, provided no member ever decides over a gap and every seq taken has its message stored (both below) |

## Decision

- **Layering.** The session decodes frames and the envelope and hands the chat service typed
  commands (`join`, `send`); the service does everything that is policy and talks to the room
  plane through `IRooms` (the router in production, a fake in its unit tests). Commands of
  other families (the E2EE key directory of M20) are dispatched by the session to their own
  service, so the chat service does not grow with the envelope. The service joins a room once
  per node, on the first client's join, and leaves it when no client here has been in it for
  30 s.
- **Rate limit.** A token bucket per user across their connections on the node, on the injected
  clock, of 10 refilling at 2 a second (the derivation is beside `ServiceLimits`). A send past
  it is answered `{"type":"error","reason":"rate_limited","id":...,"retry_after_ms":N}` and never
  reaches the room plane, so it is neither sequenced nor delivered. The existing join bucket
  (64, then 1 a second) and the per-connection bytes in flight moved into the service with it.
- **Message ids.** Every send carries an `id` (1 to 64 of `[A-Za-z0-9_-]`, unique per sender and
  room). It travels to the owner in `Send` and back out in every `Deliver` (node channel
  version 2). Each node remembers, for 60 s and at most 32768 of them, the (room, sender, id) of
  every message it sequenced or delivered and the seq it got; a send whose key is there is
  answered with that seq and not sequenced again. The node the client sends through checks
  first (it has seen the delivery if there was one); the owner checks again before each
  append, which also catches a retry queued behind its first try. A node taking a room over
  was a member of it, so it knows the keys it delivered.
- **Acks and order.** `sent` carries the id and the seq, which is the room's `last_seq` after
  that append; messages carry seq, sender and id. A room's seqs rise by one per message, and
  every client of a room sees them in that one order. A jump in the seqs a client sees is a
  gap: skipped because it was lossy, missed while away beyond what was kept, or sequenced while
  the room was between owners and never delivered (ADR-0035).
- **Resume.** Each room this node is in keeps its latest messages: 256 KiB per room and 32 MiB
  across rooms (131072 messages), oldest first. `join` with `"after":N` sends, after `joined`,
  the kept messages above N, as many of the newest as fit 128 KiB (half the backlog that closes
  a connection, so resuming cannot get a client closed), then live messages. The 128 KiB is the
  client's for all its resumes within one linger, and a resume in a room it is already in costs
  a join from the join bucket, so asking again and again makes the node encode no more. Kept messages live
  on the node that delivered them: a client that reconnects through another node, or later than
  the room lingered, gets what that node has, and sees the rest as a gap. `joined` names the
  room's head (the latest seq the owner took, from its answer to the subscription, or this
  node delivered), so the gap shows even when nothing follows it. Filling a gap is a read of
  history by seq, which M19 provides.
- **Delivery.** A durable client gets every message; one too far behind is closed at 256 KiB
  unsent (ADR-0036) and resumes. A client joining with `"delivery":"lossy"` is skipped (and the
  skip counted) while it has more than 64 KiB unsent, and stays connected. The choice is the
  client's, per join: a viewer of a busy live chat would rather skip than reconnect. Every room
  created so far is `durable` in `room_state`; when rooms of other kinds are created (with live
  streams), their row decides and the join's choice gives way to it.
- **Envelope.** The final shapes are documented in `apps/chat/src/envelope.hpp`. Bodies are
  base64url without padding; `ref` is gone, the `id` replaces it.
- **End-to-end encryption.** MLS ciphertext is a body like any other. The room owner is the
  Delivery Service by sequencing: commits and application messages share the room's order, and
  members apply the first valid commit for an epoch in seq order. Welcomes and key packages go
  through `IE2eeDeliveryService`, not through rooms. Nothing in the chat service or the router
  looks at a body, so neither changes for Phase 3. The rule "first commit in seq order wins" is
  only sound if every member sees the same prefix, which takes three things:
  - A client never applies a commit while any lower seq is missing: it fills the gap from
    history first. A member that missed the winning commit and applied a later one would fork
    the group.
  - E2EE rooms are durable only; a lossy skip of a commit cannot be recovered from.
  - No seq exists without its message: the owner stores the body in the same fenced write that
    takes the seq, so a message sequenced and never delivered (the owner died mid fan-out) is
    still in history for whoever missed it. `IRoomStore::append` already carries the message
    for that reason; Postgres stores it from M19, which must land before the phase-2 tag.
- **Memory.** Added to ADR-0036's budget: kept messages 32 MiB, remembered keys about 10 MiB.
  About 730 MiB at the very worst, in a 1 GiB pod.

## Consequences

- A user with connections on several nodes gets a bucket on each; three nodes, three times the
  rate. Accepted: the limit is against floods from one connection or script, and a global one
  would cost a round trip per message.
- A retry is recognised for a minute, or less when the node sequences or delivers more than
  about 546 messages a second (32768 keys over 60 s). The keys are one window for the whole
  node, oldest out first, so other users' traffic shortens everyone's window: many accounts,
  each within its own rate, can bring it down toward the forward timeout. No one can reach
  another's entries (a key is the sender's own, and senders are authenticated), only shorten
  how long they last. A retry through a node that never delivered the first try, to an owner
  that did not sequence it, is sequenced again; M19 closes both with a unique key per stored
  message.
- A reused id loses the second message: it is answered with the first one's seq. Clients must
  make ids unique, a UUID or ULID per message.
- Nodes of node-channel version 1 and 2 refuse each other, and log it as `version mismatch`; a
  rolling deploy from M16 has rooms unreachable across the two versions until the old pods have
  drained. When chat's deployment overlay lands, its RUNBOOK says so: a change of node-channel
  version is rolled out by recreating the pods, or by surging the new ones and draining the old
  with that outage accepted.
- The message ids are visible to everyone in the room, and so are part of what a client must
  not put anything secret into.
- No message is stored in M17, so the acceptance test's search of the database for bodies
  cannot fail yet; only its search of the logs can. At M19 it must also check that the rows
  grew by the bodies stored.
- Presence (M18) builds on the same per-node room membership; persistence (M19) adds history by
  seq for gaps, stored keys for retries across owners, membership checks on join, and the
  room's delivery kind from `room_state`.

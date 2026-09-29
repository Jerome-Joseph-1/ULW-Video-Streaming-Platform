# 0057. A stream's live chat: joined by the stream, lossy for every viewer, bounded everywhere

Status: Accepted
Date: 2026-09-29

## Context

Section 8.15 names four room kinds; `StreamLiveChat` is the chat beside a live stream (M30,
M31). It differs from the others in scale and in what a reader wants: thousands of viewers, any
of whom may be on a phone that stalls, and a chat whose value is in its last few seconds. M32
asks for a profile in which delivered messages stay in order, a slow consumer drops past a
bounded depth while the node's memory stays flat, and the other viewers are unaffected.

What was there: ADR-0043's lossy delivery skipped every new message while a client had more
than 64 KiB unsent, so a viewer that stalled read the oldest of its backlog first and lost
whatever came while it was stalled. Nothing tied a room to a stream, and ADR-0052 (M19) records
a room's kind at its first join and stores every message of every room before delivery. Below
the process, the kernel autotuned each connection's send buffer up to `tcp_wmem`'s 4 MiB, so a
viewer that stopped reading held megabytes of the pod's memory before the service noticed it
was behind at all.

## Options

How a viewer that fell behind is dropped:

| Option | Why it was tempting | Verdict |
|---|---|---|
| Drop the newest: skip while behind (ADR-0043) | No state; already there | Rejected: after a stall the viewer reads stale messages first and never sees what came during it |
| Drop the oldest from a queue per viewer | What live chat wants: the newest survive | Rejected: a copy of every message per viewer, thousands of times over, bounded only by count times size |
| Drop the oldest through a cursor per viewer into the room's kept messages | The messages are kept once per room already (for resume); a viewer costs one seq | Accepted |

How a viewer learns it missed messages:

| Option | Why it was tempting | Verdict |
|---|---|---|
| A frame saying how many were dropped | Explicit | Rejected: a new message kind for what the seqs already say; seqs rise by one per message, so a jump is the count |
| The gap in seqs, filled from history if the client cares | No new envelope; the same rule as every other gap (ADR-0043) | Accepted |

How a live chat is tied to its stream:

| Option | Why it was tempting | Verdict |
|---|---|---|
| The first join names the kind (ADR-0052's `"kind":"live"` on any room id) | Nothing to add | Rejected: any client makes an open room of any id, and a squatter can take a stream's room first as a closed group |
| A table from stream to room, written when the stream starts | Only real streams get chats | Rejected: the packager would have to reach chat's database, and a viewer who opens the page before the publisher cannot join |
| The room id derived from the stream's name, as a version 8 UUID; joined only by the name | No table; every node and client computes the same room; the id says the kind | Accepted |

Whether live chat is stored (ADR-0052):

| Option | Why it was tempting | Verdict |
|---|---|---|
| Store nothing for lossy rooms | No database cost for a large audience's chatter | Rejected: the seq and the message are one statement (ADR-0052), a seq-only path would come back for one kind, and a viewer who arrives late has no context |
| Store every message, as for other rooms | One path | Rejected: a popular stream writes tens of messages a second for hours, kept forever |
| Store every message, keeping the room's newest 1000 | Late viewers page back through the last few minutes; storage per live room is bounded | Accepted |

How senders are limited in a room of thousands:

| Option | Why it was tempting | Verdict |
|---|---|---|
| Each user's own bucket only (ADR-0043) | Already there | Rejected: 3000 viewers at 2 a second ask for 6000 a second of a room whose owner sequences about 1000 (ADR-0035); every sender gets `busy` |
| A bucket per room at the owner | One exact limit | Rejected: the router would change for a policy that belongs to the chat service, and the refusal would cost a forward first |
| A bucket per room on each node, beside the user's | Refused where the send arrives; the router is untouched | Accepted |

## Decision

- **Joining.** `{"type":"join","stream":"<name>"}`, with the stream's name as the live
  packager takes it (1 to 64 of `[A-Za-z0-9_-]`, `apps/live-packager/src/stream_id.hpp`). The
  room is the first 16 bytes of SHA-256 over `ulw-live-chat:` and the name, with the version
  set to 8 and the RFC 9562 variant (`apps/chat/src/live_chat.cpp`); `joined` names it, and
  sends and history use it like any room id. A join that names a version 8 id as `room` is
  refused `bad_room`: nothing else makes one, so a room is a live chat exactly when its id says
  so, and nobody can create it ahead of its stream as anything else. The room is open: anyone
  authenticated may join, whether or not the stream is on air.
- **Delivery.** Every viewer of a live chat is lossy, whatever its join asked; a stalled
  viewer is never closed for it. Every lossy client, in any room, is served the same way: while
  it has 64 KiB or less unsent it is pushed each message as the room gets it; past that it gets
  nothing new and keeps a cursor, the first seq it has not been sent. It is owed at most the
  room's newest 64 messages: as the room moves on, and as the room's kept messages (256 KiB)
  age out, the cursor moves past what it can no longer be sent, and each seq it moves past is
  counted in `lossy_drops_total` at once, whether or not the client ever reads again. When
  its connection's queue drains (the reactor's `on_writable`), it is sent the kept messages
  from the cursor, oldest first, until it is 64 KiB behind again or has caught up; only then
  do new messages reach it directly again, so it never sees them out of order. 64 is three
  screens of a phone's chat: what a viewer returning from a stall wants, and older is history.
- **Kernel buffers.** Every client connection's send buffer is fixed at 32 KiB, which Linux
  doubles to 64 KiB (`Limits::socket_send_buffer`). A stalled viewer holds at most that in the
  kernel instead of 4 MiB: 80 MiB over 1280 connections instead of 5 GiB. 64 KiB a round trip is
  640 KB/s at 100 ms, far above chat's traffic, and a history page's 256 KiB in four round
  trips.
- **Memory per viewer.** A lossy client costs its node at most 64 KiB of unsent output plus
  one message, 64 KiB of kernel buffer, and one seq; the messages it is owed are the room's,
  kept once, inside ADR-0043's 32 MiB. The node's budget (ADR-0036, ADR-0043) grows only by
  the kernel's 80 MiB, to 810 MiB in a 1 GiB pod, and does not grow with the audience or with
  how long a viewer stalls.
- **Senders.** A live chat message is at most 2000 bytes (500 characters of up to four UTF-8
  bytes; `too_large` beyond). Besides each user's bucket (10, then 2 a second), each node lets
  40 messages into a live chat at once and then 20 a second, from all its senders together;
  past it the send is `rate_limited` with `retry_after_ms`, and the sender keeps the token its
  own bucket gave. Three nodes make 60 a second, more than anyone reads and 18 KB/s to each
  viewer at 300 bytes a message; the owner's ceiling of about 1000 a second holds for sixteen
  nodes with room to spare.
- **Fan-out.** Unchanged from ADR-0035 and ADR-0043: the owner sequences once and sends one
  `Deliver` to each node with viewers; each node encodes each message once and hands it to its
  viewers. Thousands of viewers cost the owner one frame per node, and a node one copy per
  viewer into the socket, for the viewers that are keeping up.
- **Storage.** Live chat messages are stored like any other (ADR-0052), before delivery, and
  the room keeps its newest 1000: the append that stores seq N of a lossy room deletes seq
  N - 1000 in the same statement, one more primary key write. 1000 messages of at most 2000
  bytes are about 2 MiB a room; at the 60 a second of three nodes that is 17 s of the busiest
  chat, and minutes of an ordinary one, which is as far back as a late viewer pages.

## Consequences

- A viewer that stalls sees, when it reads again, what was already in its socket, then a jump
  in seqs, then the newest 64. It never sees a message twice or out of order, and it can fill
  the gap from history, as far back as the room still stores.
- A viewer that stops reading altogether shuts its receive window, and the kernel ends the
  connection once it has stayed shut for `TCP_USER_TIMEOUT` (20 s, `net::tune_connection`).
  Its client reconnects and joins again; the node never held more than the bounds above for
  it. A slow reader keeps its connection.
- `lossy_drops_total` counts seqs lossy clients were moved past, including gaps of the room
  itself that a behind client was waiting across. A node whose count climbs has viewers that
  cannot keep up, not a fault of its own.
- A stream's chat exists as soon as someone joins it, even for a stream that never goes on air.
  Joins are rate limited per user (ADR-0036), and every such room is bounded in storage, but
  rooms of ended streams keep their last 1000 messages. Reopen with a retention job when the
  table shows them.
- Each node's allowance is its own, so the room's total is 60 a second on three nodes and grows
  with nodes. The owner's ceiling is far above it; if chat ever runs on dozens of nodes, move
  the limit to the owner.
- A client cannot choose to be durable in a live chat: one too far behind would be closed and
  resume from the node's kept messages, which is worse than skipping for a chat nobody reads
  back. Clients that need every message read history.
- Fixing the kernel's send buffer applies to every chat connection. A client that reads fast
  but is far away is held to 64 KiB a round trip, which chat's traffic never approaches.
- The change to lossy delivery is in `apps/chat/src/chat_service.cpp`; `rt/src/room_router.cpp`
  is unchanged.

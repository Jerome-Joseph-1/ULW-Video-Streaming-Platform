# 0137. A room tells its members it moved on

Status: Accepted
Date: 2026-10-22

## Context

A socket hears a room's messages only once it has joined it, and joins at most 64 rooms (ADR-0117).
A room list of a few hundred conversations had no way to show which moved since it was opened:
`rooms` said nothing of a room's latest message, and the hook's `message` event reaches only
members with no socket at all (ADR-0111). A client polled `rooms` and joined everything it could
to keep its badges right (#196).

The room plane already reaches every socket of a user wherever it is connected: a ring is an
unsequenced notice to the callee's presence room, which every node with a socket of theirs is in
(ADR-0056, ADR-0091). Notices are opaque bytes to the node channel, so a new kind of notice needs
no change of its wire format.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| An unasked `activity` frame, from the sender's node to each member's presence room, at most once a second per room from that node | Every socket of every member hears it, joined or not; no wire change; the node that sent the message is the one place that knows of it once, as the hook's `message` event is made there | One notice per member per second of a busy room, and a member list read per room (kept until it changes) | Accepted |
| The same from the room's owner | One rate limit for the room, whichever nodes send to it | The owner has no hook of its own on a sequenced message, and need not have the room joined: a change of the room plane | Rejected |
| A notice to the room itself, to its joined sockets only | Cheap | Only reaches what joined, the problem itself | Rejected |
| `seq` and `last_at` on each `rooms` entry | A list opened, or reloaded, is right at once; one probe of two primary keys per room | Read when asked, not pushed | Accepted, with the frame |
| Unread counts on `rooms` | What a badge shows | The server keeps no read marks of its own today, and a count needs one per member | Rejected here |

## Decision

- **The frame.** `{"type":"activity","room","seq","sender","at","quiet"}`, no body, on every socket
  of every member of a direct or group chat, the sender's own included, whether they joined the
  room or not. `at` is when the owner sequenced it (the delivery's time, when it reached the
  sender's node first; that node's clock otherwise), in Unix milliseconds.
- **Rate.** The sender's node tells a room's members at once, then at most once a
  `activity_interval` (1 s) per room. Messages in between are folded into the next notice: it
  names the newest, and `quiet` is true only when every one it covers was quiet (ADR-0128). Each
  node limits its own senders, so a room with senders on three nodes is told up to three times a
  second.
- **Members.** The node reads the room's member list (up to 1024) once and keeps it while the room
  is in use here, until a member is added or removed, or the store resyncs. A list that cannot be
  read tells nobody of that message and is counted. A stream's live chat tells nobody.
- **The notice.** Layout 2 of the notices (`apps/chat/src/room_notices.cpp`): the member, the
  room, the seq, the sender, the time and the quiet flag. The ring's is layout 1; a node of an
  earlier release counts a layout it does not know as malformed and pushes nothing.
- **Listings.** Each `rooms` entry, on the socket and the service API, carries `seq` (the room's
  counter, 0 before any message) and `last_at` (when its newest stored message was sequenced, Unix
  milliseconds; absent while none is stored). Both are probes of a primary key per room, in the
  same statement as the page.

## Consequences

- A client keeps its room list right from `activity` frames, and from `rooms` when it connects;
  it joins only the rooms it shows.
- `activity` is best effort, like a ring's notice: a notice lost to an owner changing hands is
  not sent again, and the next message or a `rooms` page corrects it.
- A removed member stops hearing a room's activity from the moment its node hears of the removal.
- `activity_notices_total`, `activity_folded_total`, `activity_failures_total`,
  `activity_pushed_total` and `activity_rooms` count it.

# 0036. chat_server's client edge: envelope, limits and allocation failure

Status: Accepted
Date: 2026-09-29

## Context

chat_server (ADR-0019) takes WebSocket connections from browsers and apps. ADR-0029 left it
three decisions: how to keep a read full of tiny control frames from turning into thousands of
allocations and replies, what an allocation failure inside a `noexcept` reactor callback does,
and how to stop a foreign page from opening a socket with the user's cookie. Section 3 leaves
the chat envelope open; M16 needs only enough of one to join a room, send, and receive
sequenced messages, and M17 owns the rest (acks, resume, rate limits).

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Let an allocation failure terminate the process, as the Autobahn echo server does | The rest of the codebase treats it that way; no code | Rejected for chat: one connection's input would take every other connection on the node down with it, and each of those rooms then waits out a takeover |
| Preallocate every per-connection buffer | No allocation on the data path at all | Rejected for M16: decoded messages, JSON values and outgoing frames are all sized by the peer; pooling them is the optimisation ADR-0029 already allows later without an interface change |
| Bound what each connection can make the node hold, and catch `std::bad_alloc` where a peer's input drives allocation, closing that connection | Memory is bounded by admission, and a failure costs one connection | Accepted |

## Decision

- **Memory.** Every queue is bounded in bytes, not in entries, and admission bounds the rest:
  - a client connection holds at most one 64 KiB message in its decoder, 256 KiB of unsent
    output and 128 KiB of sends not yet answered (each counted as its body plus 256 bytes,
    wherever it waits: an owner's queue or the node channel), 448 KiB in all; 1280
    connections are 560 MiB;
  - until its one HTTP request is answered, a client connection holds the request parser
    instead: about 28 KiB of fixed buffers for the head, plus the bytes that arrive behind it,
    at most 256 KiB, in a buffer grown only as they arrive, so at worst about 284 KiB. The
    session frees the parser as it answers (bytes behind an accepted upgrade go to the frame
    decoder first), so an open socket holds none of it, and a connection still in its handshake
    holds nothing of the 448 KiB above. Each connection is in one phase or the other, and 284 KiB
    is below 448 KiB, so the 560 MiB for 1280 connections holds whatever mix of phases they are
    in;
  - an owner's queues of writes awaiting their sequence numbers hold at most 1 MiB per room and
    64 MiB across rooms, again body plus 256 bytes a write; past either, the write is `busy`;
  - at most 32 node-channel connections, each holding one frame (64 KiB) being decoded, 1 MiB
    queued before it opens and about 1 MiB unsent once open, beyond which it is closed:
    about 67 MiB;
  about 690 MiB at the very worst, inside a 1 GiB pod, with the rest for the kernel's socket
  buffers, which the pod's memory is also charged for.
- **Allocation.** The read callbacks of client sessions and of node-channel connections, and the router's answers
  to a session, catch `std::bad_alloc`, count it (`allocation_failures_total`) and close that
  connection; a session inside the router's fan-out closes on the next loop iteration, so the
  fan-out never sees its members change. The router's own timers and store answers allocate a
  few bounded vectors per event and are left to terminate, as everywhere else.
- **Control frames.** At most 8 control frames in one read, and a token bucket of 20 refilling
  at 10 a second across reads; past either, Close 1008. A Ping inside the budget is answered with
  a Pong carrying its payload. The server pings a connection quiet for 30 s and closes one quiet
  for 75 s.
- **Origin.** A token in `Authorization: Bearer` is accepted from anywhere: a browser cannot set
  that header on a WebSocket, so the client chose to send it. A token in the cookie
  (`ULW_AUTH_COOKIE`) is accepted only with an `Origin` listed exactly in `ULW_ALLOWED_ORIGINS`;
  otherwise 403, before the token is looked at. With no list configured the cookie is refused.
- **Envelope (M16).** JSON objects in text frames, documented in `apps/chat/src/envelope.hpp`:
  `join` and `send` from the client; `joined`, `sent`, `message` and `error` from the server.
  Unknown types and unknown fields are refused with `error`, not ignored. A body is a JSON
  string carried as opaque bytes: never parsed, logged or indexed, and returned re-escaped but
  byte for byte. A binary frame closes with 1003.
- **Joins.** A user may join rooms new to the connection at a burst of 64 and then one a
  second, counted across all their connections. Every such join is charged, not only those
  that create a room: a join of an unknown room creates it, and its rows outlive the room's
  use, but whether a room exists is known only after the lookup the charge is meant to limit.
  A rejoin of a room the connection is already in costs nothing. Past the bucket, the join is
  answered `busy`.
- **Slow readers.** A connection with more than 256 KiB unsent is closed. Its client reconnects
  and, from M17, resumes from its last seq.
- **Probes** share the client port: `/healthz` (the loop is alive), `/readyz` (not draining, the
  node's address is published and its owner heartbeat reaches the database) and `/metrics`.
- **Transport.** Plain TCP: Envoy terminates TLS in front of chat as it does for the gateway
  (ADR-0001). `TlsTransport` can be put in front of the sessions without changing them.

## Measurement: the parser kept after the upgrade

At first every parser reserved its 256 KiB up front and each session kept its parser for the
life of the socket. Each reservation was written only a few hundred bytes, glibc's dynamic mmap
threshold (raised past 256 KiB once one such block is freed) put it in the brk heap, and the
heap's extent followed the peak number of
concurrent sessions x 256 KiB. The reservations freed as sessions ended left untouched space in
the heap that later allocations faulted in page by page, so resident memory crept up for hours
(8 to 12 MB of never-touched heap per node on the chat soak), and 1280 connections reserved 320
MiB of address space outside the budget above. With the mmap threshold pinned at 128 KiB, so that
each reservation was mapped and unmapped on its own, the heap stayed flat after the warm-up,
which confirmed the mechanism.

Freeing the parser at the answer, with its buffer grown only as bytes are held, was measured on
a 15-minute chat soak (`--hours 0.25`) run twice side by side, three nodes each, reading `[heap]` in
`/proc/<pid>/smaps`:

| | heap extent, minute 5 | minute 15 | heap Rss, minute 15 | extent never touched, minute 15 |
|---|---|---|---|---|
| parser kept, 256 KiB reserved | 25.3 to 26.5 MB | 27.5 to 30.7 MB | 16.6 to 21.2 MB | 9.5 to 11.9 MB |
| parser freed at the answer | 12.5 to 12.9 MB | 14.5 to 17.2 MB | 14.3 to 17.0 MB | 0.2 to 0.3 MB |

The extent halves and the untouched part the creep came from is gone. Both still grow between
minutes 5 and 15, which the soak counts as warm-up (its pools fill then), so flatness after it is
for a full soak to show.

## Consequences

- A flood of pings costs the flooder its connection, not the node its memory; a client library
  that pings more than 10 times a second is cut off, which none does.
- Memory is bounded by the limits above, not by the kernel's buffers, which come on top; the
  pod's limit has to leave room for them.
- Clients must send the cookie only from listed origins; a new web front end needs its origin
  added to the deployment before it can connect.
- M17 replaces the envelope's error-only answers with acks and resume. The shapes here are not
  a compatibility promise.
- Reopen if a profile shows allocation on the chat data path in its top frames (ADR-0029).

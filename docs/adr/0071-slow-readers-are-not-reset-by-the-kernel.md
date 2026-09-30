# 0071. Slow readers are not reset by the kernel: the gateway and the node channel bound their own peers

Status: Accepted
Date: 2026-09-30

## Context

`net::tune_connection` sets `TCP_USER_TIMEOUT` to 20 s on every connection it tunes (ADR-0026
lists it). ADR-0070 found what Linux does with it once the peer's receive window shuts: it
counts from the first probe of the shut window and restarts the count only when the window
opens wide enough for the whole unsent head of the send queue, a segment of up to about half
the peer's largest window with GSO. A peer that keeps reading, but frees its window a little at
a time, is therefore reset by the kernel 20 s after it first fell behind, with no close of our
own and no counter moving. ADR-0070 cleared the timeout on chat's client connections. Two more
servers still had it:

- **The gateway's client connections** (`apps/gateway/src/gateway.cpp`): a player reading a
  long media playlist, or anything else the gateway answers, a little at a time.
- **The node channel** (`rt/src/room_router.cpp`), on the connections a node accepts and on
  those it dials: a busy node reading deliveries or forwards a little at a time was reset as
  if it had vanished, counted in `peers_lost_total` on the dialling side and in nothing on the
  accepting side, instead of being let be or counted in `slow_peers_total`.

Measured on loopback with the timeout set to 1.5 s, and a reader with 8 KiB of receive buffer
(16 KiB once Linux doubles it) reading 8 KiB every 250 ms: a player reading a 130 KB playlist
from the gateway was reset 1.75 to 2.25 s in, over plaintext and TLS, on both reactors; a node
reading its deliveries, and an owner reading forwarded writes, were reset 2.0 to 2.3 s in. With
the timeout cleared, each read to the end.

What bounded a peer that stops acknowledging while output waits for it, apart from the kernel:

- **Gateway.** A response is queued whole when the request ends, and its connection then waits
  for the next request under the header timeout (10 s, `timeouts_total{kind="header"}`), or,
  after `Connection: close`, lingers 2 s for the client's FIN. While a request is in progress,
  its own timers run: the header timeout from its first byte, the body's 30 s idle timeout and
  8 KiB/s floor, the store's bound on a body it holds up (ADR-0045), the catalog's and the
  store's own timeouts, and the six-hour backstop. But nothing stopped a client from asking
  again without reading: the gateway kept reading requests whatever it still had queued for
  the client, and each request restarted the header timeout. A client that sent a request
  every 9 s and read nothing held its connection for up to 1000 requests
  (`max_requests_per_connection`), 2.5 hours, with a response queued in the gateway for each;
  only the kernel's 20 s ended it.
- **Node channel.** Only `kMaxPeerBacklog` (sixteen of the largest frames unsent, about 1 MiB):
  a peer past it is closed as slow. Below it nothing watched: forwards that time out fail but
  leave the link up, subscriptions have no deadline, and keepalive does not probe while output
  waits. The kernel's 20 s was the only bound on a node that stopped reading, or vanished,
  with less than that queued for it.

## Options

The gateway's client connections:

| Option | Why it was tempting | Verdict |
|---|---|---|
| Keep the user timeout | It already ends a client that asks and never reads | Rejected: it resets a reader the gateway has not given up on, wherever the gateway's own timer is longer than 20 s |
| Clear it, and watch acknowledgements as chat does | The same mechanism everywhere | Rejected: once the gateway stops reading while a response waits, every state with output waiting has a timer of its own; a watch would add a system call a second per connection for a bound that is already there |
| Clear it, hold a new request back while the last response is held back by the client's window, and rely on its timers | The header timeout then runs from the last response for a client that does not read it | Accepted |

The node channel:

| Option | Why it was tempting | Verdict |
|---|---|---|
| Keep the user timeout: a node must never stall | Nodes are ours, on a private network | Rejected: a node that is only busy stalls in exactly this way, and losing its link loses every room it subscribed to until it resubscribes |
| Clear it and rely on `kMaxPeerBacklog` | Already there | Rejected: a node that stops with less than 1 MiB queued for it, or vanishes with output in flight, would hold its link until keepalive, which does not run while output waits |
| Clear it and close a link whose output waits 20 s with none of it acknowledged (`TCP_INFO`) | Counts exactly what tells a stopped node from a slow one, whatever window it opens; the same 20 s as before | Accepted |

How a gateway connection is closed with output still waiting for the client:

| Option | Why it was tempting | Verdict |
|---|---|---|
| A FIN, as before, with the user timeout cleared | The kernel still delivers what it holds | Rejected: the orphan lives as long as the peer answers probes of a shut window (ADR-0070) |
| Always a reset | Frees the kernel's memory at once and tells the peer at once | Rejected: it cuts off a response the kernel holds all of, which a slow client used to receive after a `Connection: close`, a linger or a drain, from `ffmpeg`-based players to HTTP/1.0 tools |
| A reset when the gateway still holds part of the response, which is cut off either way; otherwise a FIN with the 20 s user timeout set again for the orphan | A client still gets every response it got before, and nothing is kept without bound | Accepted |

A node-channel link is only closed with output waiting once the other node has stopped
reading, so it is always reset. ADR-0070 rejected setting the timeout again before closing for
chat's clients, which it closes only once they have stopped reading; the gateway closes clients
that may still be reading.

## Decision

- **Where the user timeout stands.**

  | Server | Connections | `TCP_USER_TIMEOUT` | What ends a peer that stops acknowledging while output waits |
  |---|---|---|---|
  | Chat | client | cleared (ADR-0070) | `stall_timeout`, 20 s without an acknowledgement |
  | Chat's room router | node channel, both ends | cleared | `peer_stall_timeout`, 20 s without an acknowledgement, or `kMaxPeerBacklog` |
  | Gateway | client | cleared while open; 20 s again on the orphan of a FIN | the header timeout, 10 s from the response it has not read; the 2 s linger; the request's own timers; the six-hour backstop |
  | Test fixtures (`tests/support/*_server.cpp`) | client | 20 s, unchanged | the kernel |

  Each server clears the timeout only where the kernel answers `TCP_INFO` (`net::send_progress`)
  on the socket; where it does not, the connection keeps the kernel's 20 s, which then bounds it
  as before. Linux has answered with the fields read since 4.6.
- **Gateway, while open.** `on_accept` clears the timeout after `tune_connection`. The gateway
  keeps reading after a keep-alive response, and decides only when a new request arrives:
  its first bytes, or a request the client pipelined behind the response. If part of the
  response is then still held back (queued in the transport, or unsent in the kernel,
  `SendProgress::unsent`), the request is kept unparsed, reading stops, and the header timeout
  keeps counting from the response, so a client that does not take it within 10 s is closed
  and counted in `timeouts_total{kind="header"}`, however many requests it sent after it. The
  transport's queue draining (`on_writable`) is noticed at once; the kernel says nothing when a
  window opens, so while a request waits the response is looked at every 100 ms
  (`kDrainCheck`), a `getsockopt` each. A client that asks again only once it has read its
  response, as a player does, finds it gone and never waits: whatever the response's size,
  and whether or not it outgrew the congestion window on the way. Only a client that asks
  before its last response has left the gateway's kernel waits, at most 100 ms after it has.
  So a client that never reads holds at most its own receive buffer, one send buffer, one
  response and the one receive that brought its next request.
- **Gateway, at close.** Whatever closes a connection (a timeout, the linger's end, the drain
  deadline, the client's EOF or error): if the transport still holds part of the response,
  the close is a reset, which drops what the kernel holds too; if only the kernel holds output
  for the client, `TCP_USER_TIMEOUT` is set back to 20 s (`net::restore_user_timeout`) and the
  close is a FIN, so the kernel finishes the response within the bound it always had; with
  nothing waiting, a plain FIN.
- **Gateway, on a drain.** An idle connection whose last response is still waiting for the
  client is given the 2 s linger to read it, as after `Connection: close`, instead of being
  closed at once; one with nothing waiting is closed at once, as before. A lingering
  connection parses nothing more, so a request the client pipelined behind its last response
  goes unanswered, as after `Connection: close`.
- **Node channel.** Both ends clear the timeout on a new connection. On every tick (250 ms),
  each authenticated accepted connection and each open dialled one whose output waits (in the
  reactor, or unacknowledged in the kernel) reads what the other node has acknowledged; one
  that acknowledged none of it for `RouterConfig::peer_stall_timeout` (20 s) is closed with a
  reset and counted in `slow_peers_total`. The watch notes an acknowledgement at the first tick
  after it and fires at the first tick 20 s after that, so a link is closed 20 to 20.5 s after
  its last acknowledgement, never before. A dialled link is then taken down as any lost link
  is: its requests fail `unavailable`, `peers_lost_total` counts it and `on_peer_lost` reports
  it. A link past `kMaxPeerBacklog` is now reset too. The handshake's 5 s and keepalive's 90 s
  for an idle link are unchanged. The watch reads `TCP_INFO` on every tick for every link,
  output queued in the reactor or not: output the kernel alone holds waits all the same.
- **Test fixtures.** The echo servers under `tests/support` keep `tune_connection` as it is:
  their peers are tests that read everything at once, and they stand for a server's socket
  setup, not for a policy on slow readers.

## Consequences

- A player that reads a response a little at a time keeps its connection for as long as the
  gateway's own timers allow, and a node that reads a little at a time keeps its link however
  long it takes, as long as some of what waits for it is acknowledged every 20 s.
- A client that asks and never reads is closed 10 s after the first response its window held
  back, where the kernel took 20 s. A client that asks again after reading its response is
  served as before. One that pipelines behind a response not yet sent in full (a large
  playlist on a link slower than the response, or an io_uring send not yet completed) has its
  next request wait until the response has gone, plus up to 100 ms when only the kernel held
  it back; in the tests, a client that reads its response first is answered with no wait.
- A response that takes more than 10 s to read on a keep-alive connection is still cut off by
  the header timeout, which counts from when the response was queued, as before; the gateway
  cuts it with a reset if it still held part of it. That is the gateway's limit, not the
  kernel's.
- An orphan left by a FIN still carries the kernel's 20 s, and with it the miscount this ADR
  is about: a client reading a response's last kernel buffer a little at a time after the
  gateway has closed can still be reset 20 s after its window first shut, as before.
- `slow_peers_total` now also counts a node that vanished with output waiting for it: it
  acknowledges nothing either, and is closed at the same 20 s the kernel used to take. On the
  dialling side such a link also counts in `peers_lost_total`, as it did.
- The watch costs a `getsockopt` per node-channel connection per tick, about 130 a second at
  the 32 accepted connections a node allows; the gateway's, one per close, one per request that
  arrives after a keep-alive response with the transport's queue empty, and ten a second per
  connection holding a request back.
- `tests/gateway/gateway_playback_test.cpp` and `tests/unit/rt/room_router_test.cpp` check that
  the server's socket has no user timeout, read a response, deliveries and forwarded writes
  8 KiB every 250 ms of wall clock for four to five seconds, and check that a reader that stops
  is reset. With the kernel's timeout at 1.5 s and the old code, each slow reader was reset
  about 2 s in; with the old code as it ran, the check of the socket fails (20000). The
  gateway's tests also cover a client that asks and never reads (the old code answered all
  eight of its requests and kept it open), a close that finds the largest playlist still in
  the gateway (a reset) or only in the kernel (the response in full, then a FIN), a drain that
  lets a client finish reading its last response, and a drain just after a response, which
  answers nothing the client pipelined behind it.

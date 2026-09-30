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
the timeout cleared, each read to the end, some four seconds.

What bounded a peer that stops acknowledging while output waits for it, apart from the kernel:

- **Gateway.** A response is queued whole when the request ends, and its connection then waits
  for the next request under the header timeout (10 s, `timeouts_total{kind="header"}`), or,
  after `Connection: close`, lingers 2 s for the client's FIN. While a request is in progress,
  output can only be an earlier, pipelined response, and the request's own timers run: the
  header timeout from its first byte, the body's 30 s idle timeout and 8 KiB/s floor, the
  store's bound on a body it holds up (ADR-0045), the catalog's and the store's own timeouts,
  and the six-hour backstop. Each of these ends the connection by the gateway's own close,
  which drops what the transport still holds. Nothing on a live connection waits on an unread
  response longer than those timers allow, so the kernel's count only ever acted on the orphan
  left after that close, where the 20 s also bounded how long the kernel kept it.
- **Node channel.** Only `kMaxPeerBacklog` (sixteen of the largest frames unsent, about 1 MiB):
  a peer past it is closed as slow. Below it nothing watched: forwards that time out fail but
  leave the link up, subscriptions have no deadline, and keepalive does not probe while output
  waits. The kernel's 20 s was the only bound on a node that stopped reading, or vanished,
  with less than that queued for it.

## Options

The gateway's client connections:

| Option | Why it was tempting | Verdict |
|---|---|---|
| Keep the user timeout | The gateway's timers already end a live connection first, at 10 s or 2 s | Rejected: it still resets a reader the gateway has not given up on wherever a timer runs longer than 20 s (a pipelined request behind an unread response, under the body's 30 s idle timeout or the backstop), and on the orphan it keeps ending a reader that is still reading |
| Clear it, and watch acknowledgements as chat does | The same mechanism everywhere | Rejected: every state with output waiting already has a timer that ends it; a watch would add a system call a second per connection for a bound that is already there |
| Clear it, rely on the timers above, and reset a connection closed with output still waiting | No new timer; the kernel keeps nothing for a peer the gateway gave up on | Accepted |

The node channel:

| Option | Why it was tempting | Verdict |
|---|---|---|
| Keep the user timeout: a node must never stall | Nodes are ours, on a private network | Rejected: a node that is only busy stalls in exactly this way, and losing its link loses every room it subscribed to until it resubscribes |
| Clear it and rely on `kMaxPeerBacklog` | Already there | Rejected: a node that stops with less than 1 MiB queued for it, or vanishes with output in flight, would hold its link until keepalive, which does not run while output waits |
| Clear it and close a link whose output waits 20 s with none of it acknowledged (`TCP_INFO`) | Counts exactly what tells a stopped node from a slow one, whatever window it opens; the same 20 s as before | Accepted |

How a peer given up on is closed:

| Option | Why it was tempting | Verdict |
|---|---|---|
| A FIN, as before | The kernel still delivers what it holds | Rejected: with the user timeout cleared the orphan lives as long as the peer answers probes of a shut window (ADR-0070) |
| Set a short user timeout again just before closing | The orphan is bounded and still delivers | Rejected: it brings back the same miscount for the orphan, and still keeps the memory for a peer that stopped |
| A reset (`net::abort_on_close`) when output still waits | Frees the kernel's memory at once and tells the peer at once | Accepted |

## Decision

- **Where the user timeout stands.**

  | Server | Connections | `TCP_USER_TIMEOUT` | What ends a peer that stops acknowledging while output waits |
  |---|---|---|---|
  | Chat | client | cleared (ADR-0070) | `stall_timeout`, 20 s without an acknowledgement |
  | Chat's room router | node channel, both ends | cleared | `peer_stall_timeout`, 20 s without an acknowledgement, or `kMaxPeerBacklog` |
  | Gateway | client | cleared | the header timeout (10 s after the response was queued), the 2 s linger, the request's own timers, the six-hour backstop |
  | Test fixtures (`tests/support/*_server.cpp`) | client | 20 s, unchanged | the kernel |

  Each server clears the timeout only where the kernel answers `TCP_INFO` (`net::send_progress`)
  on the socket; where it does not, the connection keeps the kernel's 20 s, which then bounds it
  as before. Linux has answered with the fields read since 4.6.
- **Gateway.** `on_accept` clears the timeout after `tune_connection`. No new timer: the bounds
  in the Context are the gateway's, and none of them changes. `Connection::close` makes the
  close a reset when output still waits for the peer (queued in the transport, or unsent or
  unacknowledged in the kernel), whatever closes it: a timeout, the linger's end, the drain
  deadline, the peer's EOF or error. A close with nothing waiting is an ordinary FIN.
- **Node channel.** Both ends clear the timeout on a new connection. On every tick (250 ms),
  each authenticated accepted connection and each open dialled one whose output waits (in the
  reactor, or unacknowledged in the kernel) reads what the other node has acknowledged; one
  that acknowledged none of it for `RouterConfig::peer_stall_timeout` (20 s) is closed with a
  reset and counted in `slow_peers_total`, between 20 and 20.25 s after its last
  acknowledgement, never before. A dialled link is then taken down as any lost link is: its
  requests fail `unavailable`, `peers_lost_total` counts it and `on_peer_lost` reports it. A
  link past `kMaxPeerBacklog` is now reset too. The handshake's 5 s and keepalive's 90 s for an
  idle link are unchanged.
- **Test fixtures.** The echo servers under `tests/support` keep `tune_connection` as it is:
  their peers are tests that read everything at once, and they stand for a server's socket
  setup, not for a policy on slow readers.

## Consequences

- A player that reads a response a little at a time keeps its connection for as long as the
  gateway's own timers allow, and a node that reads a little at a time keeps its link however
  long it takes, as long as some of what waits for it is acknowledged every 20 s.
- A gateway response still unread when the gateway gives up on its connection (10 s after it
  was queued on a keep-alive connection, 2 s on a closing one) is cut off with a reset, where
  the kernel used to deliver its last buffer for up to 20 s more. The transport's share was
  dropped at that close before too, so only a response that fit in the kernel's send buffer
  loses a tail it used to get. The header timeout still counts from when the response was
  queued, as before: a response that takes more than 10 s to read on a keep-alive connection is
  cut, which is the gateway's limit, not the kernel's.
- `slow_peers_total` now also counts a node that vanished with output waiting for it: it
  acknowledges nothing either, and is closed at the same 20 s the kernel used to take. On the
  dialling side such a link also counts in `peers_lost_total`, as it did.
- A reset leaves no orphan and no `TIME_WAIT` on our side; the other end learns at once and
  reconnects, as from any lost connection.
- The watch costs a `getsockopt` per node-channel connection per tick, about 130 a second at
  the 32 accepted connections a node allows, and one per gateway close.
- `tests/gateway/gateway_playback_test.cpp` and `tests/unit/rt/room_router_test.cpp` read a
  response, deliveries and forwarded writes 8 KiB every 250 ms for about four seconds with the
  gateway's clock held and the node's stall timeout at one second, and check that a reader that
  stops is reset; with the kernel's timeout at 1.5 s and the old code, each slow reader was
  reset about 2 s in.

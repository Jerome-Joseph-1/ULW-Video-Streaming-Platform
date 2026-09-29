# 0041. Datagram mode in the reactor

Status: Accepted
Date: 2026-09-29

## Context

The call milestones (ADR-0020) leave ICE, DTLS, SRTP and RTP handling to the SFU, but the
project still sends and receives UDP of its own: the media test harness and the RTP tooling that
checks what the SFU forwards. Those need a third reactor mode beside streams (mode A) and
library descriptors (mode B): the reactor owns a UDP socket, hands each datagram over with its
source address, and sends datagrams to any address.

The reactor's rules carry over: `stop_receiving` is exact, callbacks are never re-entered from
inside a call, nothing on the data path allocates, both reactors pass the same suite, and a
loop never spins. Datagrams add their own questions: what happens to one longer than the buffer,
what `send_to` does when it cannot send, and how IPv4 peers of a dual-stack socket are named.

Measured on kernel 6.18 over loopback:

- a default 208 KiB receive buffer holds 256 datagrams of up to about 200 bytes (832 bytes of
  truesize each) and 92 datagrams of 1,200 to 1,472 bytes;
- one multishot `RECVMSG`, armed on a socket with 250 datagrams queued and stopped on its first
  completion, had posted 98 datagrams before the cancel took effect;
- `SENDMSG_ZC` of 1,200-byte datagrams cost 1,640 ns of CPU each against 1,440 ns for
  `SENDMSG` (300,000 sends each way); loopback reports every zero-copy send as copied.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| One multishot `RECVMSG` per socket into a buffer group of its own | One submission per socket for its lifetime; a buffer is taken only when a datagram arrives; each completion is exactly one datagram | Accepted for io_uring |
| Single-shot receives, as for streams (ADR-0021) | Exact stop without holding anything | Rejected: one submission per datagram. ADR-0021's objection to multishot is the megabytes a stream backlog can deliver after a stop; for datagrams the excess is bounded by the ring and kept in place (below) |
| Share the stream ring (256 x 64 KiB) | No second ring | Rejected: a 2 KiB datagram would pin a 64 KiB buffer, and a burst of datagrams would starve the upload streams |
| Deliver truncated datagrams with a flag | The handler decides | Rejected: a cut RTP packet is garbage, and every handler would have to remember to check |
| `send_to` queues without bound, like the stream send queue | Never refuses | Rejected: for media a late datagram is worth less than a fresh one, and the queue is memory the kernel's send buffer already provides |
| `recvmmsg`/`sendmmsg` on epoll | Batches syscalls on the fallback | Deferred: epoll is the fallback; `recvfrom`/`sendto` keep it simple until a measurement says the fallback carries media load |

## Decision

- `IReactor` gains `attach_datagram`, `start_receiving_datagrams`, `stop_receiving_datagrams`,
  `send_to`, `begin_close`/`is_quiescent` for a `DatagramId`, and `datagram_stats`. Handlers get
  `on_datagram(SocketAddr, BorrowedBytes)`, `on_send_error(SocketAddr, int)` and `on_error(int)`.
- `SocketAddr` is a plain value in the public headers, with no Linux types. An IPv4 peer of a
  dual-stack socket is reported as IPv4, and IPv4 destinations are written v4-mapped on an IPv6
  socket, so a peer has one identity whichever socket it reaches.
- The largest datagram is 2 KiB either way: Ethernet-MTU media carries at most 1,472 bytes.
  A longer one is dropped and counted as `truncated`; `send_to` refuses one with `EMSGSIZE`.
- io_uring receives with one multishot `RECVMSG` per socket from buffer group 2: 512 buffers of
  16 + 28 + 2,048 = 2,092 bytes, 1 MiB. 512 lets two sockets drain a full default receive
  buffer of small datagrams in one batch.
- A stop cancels the multishot. Datagrams already posted are held in their ring buffers, in
  order, and delivered from the loop when receiving resumes; close gives them back. This keeps
  the stop exact without copying, and the excess is bounded by the ring, not by the socket
  backlog.
- A receive that finds the ring empty ends with `ENOBUFS`; it is counted as `ring_exhausted` and
  re-armed at the end of the iteration, once every buffer that iteration used is back. If held
  datagrams keep every buffer, it is not re-armed until one is delivered or released, so the loop
  cannot spin.
- epoll registers a datagram socket only while it receives (a queued socket error is reported
  whatever the mask, and level triggering would report it on every wait), reads with
  `recvfrom(MSG_TRUNC)` at most 64 datagrams per wakeup, and sends with `sendto`.
- `send_to` copies the payload and either takes the datagram or refuses it with `EAGAIN`, which
  the caller drops or retries. io_uring holds at most 256 sends in flight per socket (the
  default send buffer's worth of small datagrams) and 1,024 per reactor (five times the steady
  state at 1 Gbit/s of 1,200-byte datagrams); epoll refuses when the kernel's send buffer is
  full. Kernel refusals after acceptance (`EACCES`, `ENETUNREACH`, ...) arrive through
  `on_send_error` from the loop on both reactors, never from inside `send_to`.
- A receive error (an ICMP error queued on a connected socket) stops receiving and is reported
  through `on_error`; `start_receiving_datagrams` resumes.
- Sends use `SENDMSG_ZC` when a send to ourselves at startup shows the kernel accepts it with
  `IORING_SEND_ZC_REPORT_USAGE` (6.2; 6.1 has the opcode but rejects the flag). A socket
  switches to `SENDMSG` for good at the first notification that says the kernel copied anyway,
  as it always does on loopback, where the copy costs 14% more CPU than a plain send.

## Consequences

- Measured with `ulw_datagram_soak`: 5,000 peers each sending one datagram a second to one echo
  socket for 10 minutes, attribution checked by a nonce per peer, both sides on the same kind of reactor
  in one process:

  | Reactor | Round trips | Lost | Misattributed | RSS at 30 s | RSS range after |
  |---|---|---|---|---|---|
  | io_uring | 2,997,120 | 0 | 0 | 16,136 KiB | 16,124 to 16,188 KiB |
  | epoll | 3,000,000 | 0 | 0 | 9,248 KiB | 9,248 KiB |

  io_uring refused 2,380 sends with `EAGAIN`, at four moments when a parallel build delayed the
  peers' timer and one callback sent two slices of 500 peers at once, past the 1,024 in-flight
  limit; the 52 KiB step is the send pool growing to that burst once.
- Syscalls, counted with `strace -f -c` over 10 s with 50 peers sending every 100 ms (a round
  trip is four datagram operations: two sends, two receives):

  | Datagrams per peer per 100 ms | io_uring: `io_uring_enter` per operation | io_uring: all syscalls per operation | epoll: syscalls per operation |
  |---|---|---|---|
  | 1 (19,800 operations) | 0.033 | 0.057 | 1.25 |
  | 4 (79,200 / 84,000) | 0.011 | 0.017 | 1.08 |
  | 8 (158,400) | 0.008 | 0.011 | about 1 (strace slowed the loop enough to drop datagrams) |

  io_uring's syscalls follow the number of loop iterations, not datagrams; epoll pays one
  `recvfrom` or `sendto` per datagram plus the `EAGAIN` that ends each read loop.
- A socket stopped for long holds up to the datagrams that were posted before its cancel took
  effect (98 in the measurement), out of 512 buffers shared by every datagram socket. A handler
  that stops receiving for long should close the socket instead.
- The zero-copy path runs only on routes where the kernel really avoids the copy, and nothing
  in CI has such a route. Its benefit for datagrams of 2 KiB or less is unmeasured: the kernel's
  own guidance puts the break-even around 10 KB.
- Monitor `ring_exhausted`, `truncated` and `send_refused` per socket. A steady rate of any of
  them means the ring, the size limit or the in-flight limit is wrong for the load.
- Reopen if media datagrams exceed 2 KiB, if the epoll fallback has to carry media at rate, or if
  zero-copy is measured on a real NIC and loses.

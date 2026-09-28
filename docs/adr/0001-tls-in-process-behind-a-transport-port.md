# 0001. TLS in-process behind a transport port

Status: Accepted
Date: 2026-09-28

## Context

The gateway has to run in two shapes: behind Askedin's Envoy Gateway, which terminates TLS at
the cluster edge, and on its own, where it terminates TLS itself. In both, the reactor owns the
socket and hands the session bytes it has already read (ADR-0010), so the TLS library must never
read or write the descriptor. The gateway lives on a box with at most 1 GB of RAM (ADR-0007);
every extra process and every per-connection copy is paid out of that budget.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| stunnel or a TLS sidecar in front of a plaintext gateway | No TLS code in the gateway; certificate reloads handled by a tool built for it | Rejected: an extra hop and an extra process to budget on a 1 GB box, with per-connection state held twice |
| OpenSSL bound to the socket with `SSL_set_fd` | Shortest path to working TLS; OpenSSL does its own reads and writes | Rejected: couples OpenSSL to the descriptor and to blocking-style retry semantics, so the reactor no longer owns the socket and io_uring cannot hand over the bytes |
| OpenSSL 3 with memory BIOs behind an `ITransport` port | The reactor keeps the socket; ciphertext in, plaintext out, in the same process | Accepted |

## Decision

TLS runs in-process with OpenSSL 3 in the memory-BIO model. The reactor delivers ciphertext, the
transport writes it into the read BIO, and whatever OpenSSL produces is drained from the write BIO
into the reactor's send queue. Two implementations sit behind one `ITransport` port:
`PlainTransport` passes bytes through, `TlsTransport` runs the handshake and the record layer. The
reactor and the session state machine never know which one they hold.

In the Askedin deployment Envoy terminates TLS and the gateway runs `PlainTransport` inside the
cluster. `TlsTransport` must still pass the same parameterised transport suite, because
deployments without Envoy depend on it.

## Consequences

- One extra copy per record, between the reactor's buffer and the BIO.
- TLS state costs about 35 KB per connection. The admission budget counts it (ADR-0007) even in
  the Envoy deployment, where it goes unused, so the limit holds for both shapes.
- Production does not exercise `TlsTransport`; the shared suite is the only thing keeping it
  correct.
- When the gateway terminates TLS itself, certificate reload and handshake CPU are its problem.
  Handshakes run on the reactor thread, so monitor handshake rate against loop latency there.
- Reopen if kernel TLS offload becomes necessary to meet the CPU budget: it moves the record
  layer into the socket and changes what the transport port owns.

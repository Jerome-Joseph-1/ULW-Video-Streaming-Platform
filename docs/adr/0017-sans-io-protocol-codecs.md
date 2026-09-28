# 0017. Protocol codecs are sans-IO

Status: Accepted
Date: 2026-09-28

## Context

The platform parses several wire protocols from untrusted peers: HTTP/1.1 through llhttp,
WebSocket for chat and signalling, SDP at the signalling boundary, and RTP/RTCP in test tooling.
Input arrives split at arbitrary byte boundaries, and the same code has to run on either reactor
(ADR-0010). Parsers of untrusted input need fuzzing, and fuzzing needs a parser that can run
without a socket.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Codecs that own the socket and read from it | One object per connection; fewer layers | Rejected: tests need live sockets, fuzzing needs a fake IO layer, and each codec is tied to one reactor |
| Third-party protocol libraries with built-in IO (libwebsockets and similar) | Mature and feature-complete | Rejected: they bring their own event loop and buffer ownership, which conflicts with the reactor owning the socket |
| Sans-IO codecs: bytes in, events out | Testable one byte at a time; fuzzable; indifferent to the reactor | Accepted |

## Decision

Protocol codecs (WebSocket, SDP, RTP/RTCP, and the HTTP parser wrapper around llhttp) are sans-IO.
The caller feeds bytes; the codec returns events or an error, and writes any output into a buffer
the caller provides. A codec has no reactor, no sockets, no timers and no policy: limits are
parameters, and what to do about a violation is the caller's decision.

`tools/check-boundaries.sh` fails the build if `codec/` includes anything from `rt/`, `infra/` or
`apps/`.

## Consequences

- Every codec can be tested by feeding a valid stream one byte at a time and asserting the same
  events as a single feed, and can be run under libFuzzer (`ULW_FUZZ`) without scaffolding.
- The caller writes the glue: reading from the reactor, handling events, applying backpressure.
- Codecs cannot act on time. WebSocket ping timeouts and similar are timers the caller arms.
- Events carry `string_view`s into the caller's buffer, valid only until the next feed; holding
  one longer is a bug.
- Reopen if a protocol we need exists only as a library that owns its IO, and rewriting it would
  cost more than adapting to it.

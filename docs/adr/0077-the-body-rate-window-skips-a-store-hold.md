# 0077. The body rate window skips a store's hold and keeps the bytes it counted

Status: Accepted
Date: 2026-10-01

## Context

A chunk body must average 8 KiB/s over each 30 s window the gateway spends reading it
(`min_body_bytes_per_second`, `body_rate_window`), and time the store holds the body up does
not count (`gateway.hpp`). ADR-0045 made the gateway stop reading while the store holds a body
up, and has both the idle timer and the rate window "start afresh" once it reads again.

For the rate window, starting afresh meant `restart_rate_window()` each time the parser
resumed: a new window from that moment, with its byte count set to zero. The store holds a body
up whenever its buffer is full, which for a client sending faster than the store takes bytes is
after every read: the S3 session's 64 KiB buffer fills, the rest is staged, and the parser
resumes once libcurl has taken it. Each of those resumes threw away what the client had sent
in the window so far, while the hold itself lasted no time at all.

So a client that sent fast, was held for an instant, and then slowed to a legal rate was judged
on its slow part alone. `GatewayStoreQueue.UploadsWaitingForAStoreConnectionAreHeldBackNotFailed`
showed it, about once in 200 runs under load: a leading upload sent 512 KiB at once and then
100 KiB every 10 s, 23.7 KiB/s over the window, and got `408` at 30 s. Its last resume had come
just after the 512 KiB were read, so the window held only the two later steps, 200 KiB against
the 240 KiB due.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Restart the window on every resume (as before) | The hold cannot count against the client | Rejected: the bytes the client sent before the hold are dropped with it, and a client the store holds after every read is judged on the reads after the last hold only |
| Leave the window alone on resume | Nothing is dropped | Rejected: the hold counts as reading time. A client held for 25 s of a window, which ADR-0045 lets the store do, would then owe 30 s of bytes after 5 s of reading |
| Move the window's start on by the time the parser was paused, and keep its bytes | The window covers reading time only, and every byte read in it | Accepted |

## Decision

- The connection records when its parser pauses. When the store takes the staged bytes and
  reading resumes, the rate window's start moves forward by the time since then; its byte count
  stays. The window therefore spans only time the gateway spent reading, as `gateway.hpp`
  states, and counts every byte the client sent in it.
- The idle timer still starts afresh on resume, as ADR-0045 has it: idleness is about the time
  since the last progress, which the store's taking bytes is.
- This amends ADR-0045's "both start afresh" for the rate window only.

## Consequences

- A client that sends a burst and then slows is judged on the average over its reading time,
  burst included: `GatewayUpload.BytesSentBeforeTheStoreHeldTheBodyUpCountTowardTheMinimumRate`.
- A hold still costs the client nothing: a client held for 25 s and then sending exactly the
  floor is not timed out (`GatewayUpload.TimeTheStoreHeldTheBodyUpDoesNotCountTowardTheMinimumRate`).
- A client the store holds after every read is now judged on whole windows of reading, where
  each resume used to restart its window and put the check off for another 30 s.
- The in-memory store gains `wake_writers()`, so a test can end a hold it began with
  `accept_zero`.

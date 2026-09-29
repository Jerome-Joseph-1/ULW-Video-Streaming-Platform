# 0029. WebSocket messages own their payload

Status: Accepted
Date: 2026-09-29

## Context

ADR-0017 expects codec events to carry views into the caller's buffer, valid until the next
feed. The WebSocket decoder cannot hand out such views: client frames arrive masked, so every
payload byte has to be copied once to be unmasked, and a message split into fragments has to be
joined somewhere. The decoder's interface is `feed(bytes)` returning the frames those bytes
completed, and one feed can complete many messages. The decoder must also hold no more than one
frame header plus the message being reassembled, up to its limit.

The chat service is the only consumer. Message bodies are opaque and go to the owning node, the
room's queue and Postgres; each of those keeps a copy past the read that delivered them.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Views into a decoder buffer, valid until the next feed | Matches ADR-0017; no allocation per message | Rejected: the buffer must hold every message one feed completes, so memory follows the read size rather than the message limit |
| Pull API: `feed()` then `next()` one frame at a time, each view valid until the following `next()` | Bounded memory and no allocation per message | Rejected: diverges from the specified `feed(span) -> vector<Frame>`, and the chat service would copy each body out anyway |
| Each delivered `Frame` owns its payload; the reassembly buffer is moved into it | One copy per byte (the unmask), bounded decoder memory, and a body the caller can keep and move on | Accepted |

## Decision

`codec::ws::Decoder::feed()` returns `Decoded { std::vector<Frame> frames; std::optional<CloseCode>
error; }`. A `Frame`'s payload is a `std::vector<std::byte>`: for a data message it is the
reassembly buffer itself, moved out; for a control frame, a copy of at most 125 bytes. The decoder
keeps a 14-byte header, a 125-byte control payload and the message in progress, whose capacity is
capped at the limit. Frames that come before a protocol error in the same feed are still
delivered, ahead of the error.

The default message limit is 64 KiB (four times the largest chat message, with room for an SDP
offer); a caller can raise it, as the Autobahn echo server does to 16 MiB.

## Consequences

- One allocation per data message and per control frame. At chat rates this is noise; a hot
  binary stream would want a pooled buffer, which the move-out design allows without changing
  the interface.
- The ADR-0017 rule that events are views holds for the HTTP parser and the other codecs; the
  WebSocket codec is the exception because of masking and reassembly.
- A connection that stalls mid-message holds at most the limit, never more, whatever the size of
  the reads that brought it.
- Reopen if a profile shows per-message allocation in the chat server's top frames.

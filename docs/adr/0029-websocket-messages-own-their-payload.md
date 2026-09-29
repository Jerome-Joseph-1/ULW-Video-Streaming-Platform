# 0029. WebSocket messages own their payload

Status: Accepted
Date: 2026-09-29
Amends: 0017 (its consequence that events are views, for this codec only)

## Context

ADR-0017 expects codec events to carry views into the caller's buffer, valid until the next
feed. The WebSocket decoder cannot hand out such views: client frames arrive masked, so every
payload byte has to be copied once to be unmasked, and a message split into fragments has to be
joined somewhere. The decoder's interface is `feed(bytes)` returning the frames those bytes
completed, and one feed can complete many messages. The decoder itself must hold no more than one
frame header plus the message being reassembled, up to its limit.

The chat service is the only consumer. Message bodies are opaque and go to the owning node, the
room's queue and Postgres; each of those keeps a copy past the read that delivered them.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Views into a decoder buffer, valid until the next feed | Matches ADR-0017; no allocation per message | Rejected: a view dies at the next read, but a reassembled message is complete only after the read that carries its last fragment, and the chat service keeps the body well past that (forwarding, queueing, persisting); every caller would copy it out, and the decoder would still need a buffer per message completed in one read |
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
- This amends ADR-0017's consequence that events are views valid until the next feed: it still
  holds for the HTTP parser and the other codecs, but the WebSocket codec delivers owned
  payloads because of masking and reassembly.
- The bound is on the decoder, not on what one read returns. A connection that stalls
  mid-message holds at most the limit in the decoder. But one read can complete many small
  frames: a 64 KiB read of 7-byte pings is 9,362 Frames, about 1.2 MB of vectors and
  allocations. The chat service must cap or rate-limit control frames per read (and per
  second), closing with 1008 past the cap, rather than answering each Ping it is sent.
- Decoding and encoding allocate, so an allocation failure inside a `noexcept` reactor callback
  terminates the process. The Autobahn echo server in tests/support accepts that; the chat
  server has to decide it deliberately (preallocated per-connection buffers, or admission that
  keeps allocation failure out of reach) when it is built.
- Reopen if a profile shows per-message allocation in the chat server's top frames.

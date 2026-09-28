# 0012. Realtime media is a separate tier that proxies bytes

Status: Accepted
Date: 2026-09-28

## Context

ADR-0002 keeps VOD bytes off our nodes by letting object storage serve them. Calls cannot follow
that rule: media is live and interactive, and no store sits in the path. Calls are always relayed
through an SFU, never peer to peer, so a node with a 600 Mbit/s port carries every media byte.
Capacity per node, by bandwidth:

- Usable: 600 Mbit/s less 10% for RTCP, retransmissions, signalling and the node's other traffic
  = 540 Mbit/s in each direction.
- A 1:1 call at 700 kbps each way: the SFU receives 2 x 700 kbps and sends 2 x 700 kbps, so
  1.4 Mbit/s per direction per call; 540 / 1.4 = about 385 calls.
- A 6-person room: each participant receives the other 5, so 6 x 5 = 30 downstream legs at
  500 kbps = 15 Mbit/s out per room (only 6 x 500 kbps = 3 Mbit/s in); 540 / 15 = 36 rooms.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Peer-to-peer media for 1:1 calls | No server bandwidth for the most common call | Rejected: each participant learns the other's IP address, nothing can be recorded or moderated server-side, and restrictive NATs need a relay anyway |
| An MCU mixing one stream per participant | Least downstream bandwidth per participant | Rejected: decoding and re-encoding every stream costs CPU per room far beyond two VPS nodes |
| An SFU as its own tier, forwarding without transcoding | One place for access control, recording and moderation; CPU per packet is small | Accepted |

## Decision

Realtime media is a separate tier, the SFU (ADR-0020), and it does proxy bytes. ADR-0002 applies
to VOD and live HLS only. Capacity is planned per node from both bandwidth and CPU: the figures
above are the bandwidth ceiling, SFU CPU is measured under load at those counts, and whichever
binds first is the limit.

## Consequences

- Calls compete with uploads and playlist traffic for the same port; a full call load uses all
  540 Mbit/s.
- Room size multiplies cost: downstream legs grow as n x (n - 1), 30 at six participants and 90
  at ten. Lower simulcast layers reduce the per-leg bitrate.
- Monitor egress per node against 540 Mbit/s, packet loss, and SFU CPU.
- Reopen if rooms must be larger, or bitrates higher, than two nodes can carry; the options then
  are a dedicated media node or a hosted SFU.

# 0020. LiveKit as the first SFU

Status: Accepted
Date: 2026-09-28

## Context

Calls go through an SFU (ADR-0012), and media enters through STUNner (ADR-0013). An SFU has to
handle ICE, DTLS-SRTP, RTP/RTCP, simulcast and congestion control, and each of those is a project
of its own. None of it is where ULW differs from other platforms. The rest of the project is C++.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| LiveKit | Go, Apache-2.0; driven over a network API with no in-process FFI; a documented STUNner integration; ingest and recording components exist | Accepted as the first integration |
| mediasoup | A C++ media worker, fast, in our language | Rejected: the worker is driven by a Node.js or Rust control plane that we would have to reimplement or run beside it |
| A hand-written SFU in C++ | Full control; one language | Rejected: ICE, DTLS, SRTP, congestion control and simulcast are each a long project, and none is our product |

## Decision

- Integrate LiveKit first. Core sees it through `IMediaRoom` and `IMediaParticipant`; no LiveKit
  type appears in `core/`.
- ICE, DTLS, SRTP, RTP/RTCP handling and congestion control belong to the SFU. The project's own
  sans-IO codecs (ADR-0017) cover the signalling boundary (SDP parsing and validation, the
  WebSocket signalling envelope) and RTP/RTCP parsing for test tooling and pcap verification.
- If LiveKit blocks a milestone, a superseding ADR switches SFUs behind the same port.

## Consequences

- A Go service in a C++ project: its own releases, configuration, security advisories, and CPU
  and memory profile on the nodes.
- Room and participant control goes over LiveKit's server API, one network call per join or
  leave.
- Congestion control and simulcast layer selection are LiveKit's; we tune them through
  configuration, not code.
- The port must not assume LiveKit's tokens or room naming, or switching SFUs stops being a port
  change.
- Monitor LiveKit's API latency and error rate, and the version deployed.
- Reopen if LiveKit blocks a milestone.

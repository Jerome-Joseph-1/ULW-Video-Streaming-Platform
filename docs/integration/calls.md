# Calls

> **Draft: changes until milestone M26 merges.** Nothing on `main` serves calls yet. This page
> states the model only; endpoints and message shapes will be added when they exist.

Calls go through LiveKit as the SFU (ADR-0020), with media entering the cluster through STUNner
(ADR-0013), and are always relayed. The service does not expose LiveKit's server API: after
authenticating the caller with their Askedin token and checking they may join the call, it mints
a short-lived LiveKit access token (the ticket) for that call's room, server side. The client
then connects with the LiveKit JS SDK using that ticket, and LiveKit owns ICE, DTLS-SRTP, RTP
and congestion control from there. TURN credentials are minted per session on the same
authenticated path. How a client asks for a ticket, and over which connection, is not fixed yet.

## Capacity

Measured, not modelled (tests/load/call_capacity, e2e run 36589275099): one 1:1 call, each side
publishing and receiving audio and 720p video, costs the SFU 0.0158 cores and 1.504 Mbit/s in
each direction. CPU binds first. At the 2-core limit of the stage LiveKit deployment that is
about 126 concurrent 1:1 calls per SFU pod; the 540 Mbit/s bandwidth ceiling of ADR-0012 would
allow 359 at the measured rate (385 at the 1.4 Mbit/s that ADR assumed), so it is not what
limits a pod. Rooms of more than two cost more per call (downstream legs grow as n x (n - 1),
ADR-0012); the figures above do not cover them.

ADR-0012's bandwidth arithmetic stands as the ceiling, and it defers CPU to measurement; the
0.0158 cores per call is that measurement. The run used a shared CI runner, so treat the CPU
figure as an estimate to firm up on a bigger one rather than a guarantee.

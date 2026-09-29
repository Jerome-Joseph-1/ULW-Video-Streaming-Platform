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

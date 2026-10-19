# 0127. Relay credentials are asked for again

Status: Accepted
Date: 2026-10-19
Amends: ADR-0109 (a ticket says when its relay credentials expire, and a socket may ask for fresh ones)

## Context

A ticket's `ice_servers` credentials last `ULW_TURN_TTL_SECONDS` (4 hours by default) and were
never renewed (ADR-0109). A call longer than that lost its relay when the TURN service stopped
refreshing its allocation, and the client could not tell it was coming (#203).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A socket command for fresh credentials, and `ice_expires_at` on the ticket | The client chooses when; the same issuer as tickets; no new state | A client must act on it | Accepted |
| Push fresh credentials to the call's sockets before expiry | Nothing for a client to ask | A timer per participant on some node, and the owner does not know which sockets are in the call | Rejected |
| Longer TTLs | Nothing to build | Each credential admits its holder for longer, and still runs out | Left to the operator |

## Decision

- A ticket with `ice_servers` also carries `ice_expires_at`, the time the asking node computes
  from the configured TTL.
- `{"type":"ice_servers","room","device"}` on the socket, for a room it joined, is answered
  with fresh `ice_servers` and their `ice_expires_at`. They are issued by the same
  `ITurnCredentials` that tickets use, for that user and device. It is charged as a join, since
  Cloudflare's issuer makes an API call per issue.
- Without an external TURN service it is answered `no_relay`.

## Consequences

- A client that owns its peer connections swaps the servers and restarts ICE mid-call.
  livekit-client does not expose them, so a client on it rejoins with a fresh ticket shortly
  before `ice_expires_at`.
- `ice_expires_at` may be up to a round trip later than the credentials' own expiry: ask a few
  minutes before it.

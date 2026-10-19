# 0116. A browser may ask to be refused on the socket

Status: Accepted
Date: 2026-10-19

## Context

Chat refuses a bad or expired token, an origin it does not allow, a limit, or a key server it
cannot reach with an HTTP status before the upgrade: `401`, `403`, `429`, `503`. A browser hides
that status from the page. The WebSocket only reports close code `1006`, the same as a network
failure, so a browser client could not tell "sign in again" from "network down". It refreshed its
session and retried with backoff on every `1006` (#205).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Always accept and close with a code | One behaviour | Changes what native clients, load balancers and probes see today: a `101`, not a `401` | Rejected |
| Accept and close with a code when the client asks (`/rt?refusals=close`) | Additive; browsers ask, everyone else sees what they saw | A refusal costs an upgrade's bytes | Accepted |
| A separate preflight endpoint that says whether the token is good | No change to the upgrade | A second request, a race between it and the upgrade, and a second place to keep the rules | Rejected |

## Decision

- An upgrade whose query has `refusals=close`, and whose handshake is valid, is refused for its
  token, origin, limits or key server on the socket. It gets the `101`, then a Close frame at
  once, then FIN:
  - `4001 unauthorized`, the code an expired token's socket already gets;
  - `4003 forbidden`;
  - `4029 retry_after_ms=<n>`;
  - `4503 unavailable`.
- A malformed request (`400`, `426`, `404`) is an HTTP status either way: the page cannot be
  trusted with a socket it did not ask for correctly.
- `upgrades_refused_on_socket_total` counts them.

## Consequences

- Browser clients can sign in again on `4001`, stop on `4003`, and back off as told on `4029`
  or `4503`. Native clients and probes see nothing new.
- A refused browser upgrade costs a `101` and a Close frame, under 300 bytes; the connection
  closes as a refused one does.

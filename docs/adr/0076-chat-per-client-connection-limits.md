# 0076. chat_server holds each address and each user to a share of its connections

Status: Accepted
Date: 2026-09-30

## Context

chat_server takes at most 1280 client connections, the number its memory budget allows
(ADR-0036). That was its only connection limit. The security review of 2026-09-30 (finding 5)
showed what that leaves open: one address can hold every slot through the handshake phase (each
connection costs nothing to open and is held for `handshake_timeout`, then reopened), and one
valid token can hold every upgraded socket, so a single client locks every other user out of
the node. The gateway had the same exposure and closed it with per-address and per-user limits
(ADR-0052); chat needs the same, with chat's own answer to what counts against a user.

## Options

| Question | Option | Verdict |
|---|---|---|
| What a direct peer's address is held to | Open connections and new connections a second, reset at accept before a byte is read, as the gateway's | Accepted: one address cannot fill the node, and a reconnect loop is slowed where it is cheapest to refuse |
| | Only the handshakes in flight | Rejected: a direct peer's authenticated sockets would then be limited by the user cap alone, which a script with many accounts gets round; the gateway counts direct connections for their life too |
| A customer with many IPv6 /64s | Per /64 only | Rejected: a provider may delegate a /56 or a /48 to one site (RFC 6177); a /56 is 256 /64s at 20 sockets each, more than the node's 1280 |
| | Also count the direct peers' open connections per /48, all its /64s together, in the same hold as the /64's | Accepted: a /48 is the widest delegation to one site, so one customer gets a fixed share however its provider carves its prefix |
| | Per /56 | Rejected: a customer delegated a /48 has 256 /56s, and would fill the node again |
| Behind a trusted proxy | Count the forwarded address for the socket's life | Rejected: every socket through Envoy is one connection per client, and a carrier-grade NAT puts hundreds of users on one address; their sockets would share 20 |
| | Count the forwarded address only while its upgrade is unanswered (the handshake phase, token check included), as the gateway counts a request until it is authenticated | Accepted: the handshake phase is the work done for nobody in particular; once the token is verified, the user's cap governs |
| Refusing a forwarded client or a user | Reset | Rejected: behind the proxy the connection is the proxy's, and the refusal is for one client of it |
| | `429` with `Retry-After` | Accepted: the upgrade has been read, and a status tells the client what to do |
| Per-user cap | Per node, in memory | Accepted: the node's slots are what it protects; a cross-node count would need the database on every upgrade |
| Where the shared code lives | Copy the gateway's table, hash and `X-Forwarded-For` reading into chat | Rejected: two copies of security code drift |
| | Move them to `http` (`http/client_limits.hpp`), which both already link | Accepted: the gateway uses them unchanged, by the same names |

## Decision

- **Direct peers** (not in `ULW_TRUSTED_PROXIES`): at accept, before the socket is tuned or a
  session made, the peer's address (IPv4, or IPv6 /64) is checked against
  `ULW_MAX_CONNECTIONS_PER_IP` (default 20) open connections and
  `ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND` (default 10, 10 saved); past either, the connection is
  reset (`SO_LINGER` 0) and counted in `connections_rejected_total{reason="ip_connections"}` or
  `{reason="ip_rate"}`. The count is held until the session closes.
- **IPv6 blocks.** A direct IPv6 peer's connection also counts against its /48, all the /64s in
  it together: at most `ULW_MAX_CONNECTIONS_PER_IP_BLOCK` open (default four times
  `ULW_MAX_CONNECTIONS_PER_IP`, 80), checked at accept with the /64's count and before the /64's
  rate is charged; past it the connection is reset and counted in
  `connections_rejected_total{reason="ip_block"}`. The /48's count is taken and given back with
  the /64's, in the same hold: at accept, when the session closes, and when the connection is
  refused after admission for its socket or a full node. IPv4 has no such block: its one
  address is already the client, and a /24 is often many unrelated customers.
- **Behind a trusted proxy**, the client is the `ULW_TRUSTED_PROXY_HOPS`-th `X-Forwarded-For`
  entry from the right (ADR-0052's rule, same code). Its address is held to
  `ULW_MAX_CONNECTIONS_PER_IP` upgrades not yet answered, a token waiting on a key refetch
  included; past it the upgrade is answered `429`, `Retry-After: 1`
  (`upgrades_limited_total{limit="ip"}`). The count is given back when the upgrade is answered,
  whatever the answer.
- **Per user**, at the upgrade, once the token has verified: at most `ULW_MAX_SESSIONS_PER_USER`
  (default 16) open sockets on the node; past it `429`, `Retry-After: 5`
  (`upgrades_limited_total{limit="user_sessions"}`). The socket counts until it closes.
- **Defaults, from the budget.** 1280 connections per node (ADR-0036). 20 per address is
  ADR-0052's household behind one NAT; 64 such addresses would be needed to fill a node. 80 per
  IPv6 /48 is four such households; 16 /48s would be needed. 16 per user is a phone, a laptop
  and a few tabs twice over; 80 users would be needed to fill a node, where one sufficed before.
  10 new connections a second is the gateway's, and a chat client reconnects far less often
  than a browser opens HTTP connections.
- **Tables.** Addresses: `max(16384, max_connections + 1)` entries (about 1.2 MiB); users and
  IPv6 /48s: `max_connections + 1` each (their entries hold nothing but a count, so only open
  sockets need one). All are `http::BoundedTable` over a hash seeded per process, never
  allocating once full and never dropping a counted entry. `rate_limit_entries{table="client"}`,
  `{table="user"}` and `{table="ip_block"}`, and `rate_limit_evictions_total{table="client"}`
  show them.
- The test harnesses that open every connection from 127.0.0.1 (the chat cluster and the chat
  soak) raise the per-address limits, as the gateway's load tools do.

## Consequences

- One address or one account can no longer take a chat node from everyone else. Many addresses
  or many accounts still can, up to 1280; that is Envoy's and the product's to stop. One IPv6
  customer, however many /64s it is delegated, holds at most a /48's 80 sockets.
- Clients reaching chat directly behind one NAT share its 20 sockets. Through Envoy they share
  only the handshakes in flight.
- Unrelated customers whose provider puts them in one /48 share its 80 direct sockets; a
  provider that delegates /64s from a shared /48 to many sites makes them one client here.
  `ULW_MAX_CONNECTIONS_PER_IP_BLOCK` raises the share where that matters.
- A user with more than 16 sockets on one node is refused the 17th until one closes; spread over
  nodes by the load balancer, a user can hold 16 on each.
- Every limit is per node.
- A wrong `ULW_TRUSTED_PROXIES` makes every client look like Envoy: direct counts then apply to
  Envoy's address, and `connections_rejected_total{reason="ip_connections"}` rises at once.

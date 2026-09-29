# 0052. Per-client limits, refused before the handshake, and dropping root

Status: Accepted
Date: 2026-09-29

## Context

Brief 8.13 and 8.14 ask for rate limits as token buckets `{tokens, last_refill}` (20 connections
and 10 new connections a second per address, 300 requests a minute and 3 concurrent uploads per
user, a daily byte quota), `X-Forwarded-For` trusted only from configured proxies, slowloris and
flood protection, and a privilege drop that is verified. M13 is done when a slowloris script and
a flood script both fail to degrade the p99 of a legitimate client.

Before this change the gateway had only process-wide limits: 448 connections, 448 upload slots,
3 slots per user, and the header, body-idle and body-rate timers. Measured on a local gateway
serving TLS (method below), one host was enough to hurt everyone else:

- 500 slowloris connections from one address took all 448 connection slots for the 10 s the
  header timer allows, and the legitimate client lost six requests in ten;
- a flood of 500 new TLS connections a second from one address multiplied the legitimate
  client's p99 by 6.5: every connection costs the single reactor thread a handshake.

## Options

| Question | Option | Verdict |
|---|---|---|
| Refusing a connection over its address's limit | Answer `503` with `Retry-After` | Rejected: on a TLS listener an answer needs the handshake first, which is the very cost being refused |
| | Close it | Rejected: our side keeps the connection in TIME_WAIT for 60 s, and a flood of refusals fills the port range with them |
| | Reset it (`SO_LINGER` 0) before reading a byte | Accepted: no handshake, no TIME_WAIT, and the peer knows at once |
| Where the client address comes from | The socket's peer | Accepted where the peer is the client |
| | `X-Forwarded-For`, from any peer | Rejected: any client could name any address and walk past every limit |
| | Walk from the right past every entry inside the trusted blocks, from peers in `ULW_TRUSTED_PROXIES` only | Rejected in review: on K3s, ServiceLB's masquerade or the cni0 bridge (10.42.0.1) can make Envoy see its downstream inside the trusted block, and the walk then steps over Envoy's own entry onto one the client wrote, letting it dodge its limits or spend a victim's |
| | The entry `ULW_TRUSTED_PROXY_HOPS` from the right, from peers in `ULW_TRUSTED_PROXIES` only | Accepted: each proxy appends one entry, so the Nth from the right is the one the outermost proxy wrote; a wrong count, too few entries or a malformed one count the request against the proxy itself, which fails closed |
| What an address counts as | The whole address | Rejected for IPv6: a client holds a /64 at least and could take 2^64 fresh buckets |
| | IPv4 address, IPv6 /64 | Accepted |
| Per-address limits behind the proxy | The connection limits, per connection | Rejected: Envoy's pooled connections carry any client's requests, and all come from Envoy |
| | Requests in flight per forwarded address (20), `429`, for as long as they last | Rejected: a carrier-grade NAT puts hundreds of users on one address, and its busiest few would lock out the rest |
| | The same, only until the request is authenticated | Accepted (a request waiting on a key set refetch has not been authenticated yet, so it still holds its address until the keys arrive): a verified user is held to the per-user limits instead; what the address limit guards is the work done for nobody in particular. The new-connection rate is left to direct peers, since behind Envoy the handshake it protects is Envoy's |
| Byte quota over | Declared `size_bytes` at create | Rejected: create-and-cancel would spend it without a byte sent, and the store's cost is the bytes |
| | Each `PATCH`'s `Content-Length`, charged at admission, with what never arrives given back when the request ends | Accepted: refused before a byte is read, and what is kept is what was sent |
| Quota refusal | `413` | Rejected: the request is not too large, the user is over a rate |
| | `429` with `Retry-After` from the bucket | Accepted: the same answer, and the same client action, as every other rate |
| Concurrent uploads per user across replicas | Count the user's claimed uploads in the claim's query | Rejected here: the catalog port's claim can only answer "held by someone else", which the gateway already turns into `409` and a resume; refusing a user's fourth upload needs an answer the port does not have, and the port is not this change's to alter |
| | Per process, and say so | Accepted: the slots protect each process's memory and store connections, which is where 3 is derived from; a user can hold at most 3 per replica, 6 with the two replicas prod runs, and the replica count is fixed (no HPA) |
| Memory for the per-address and per-user state | `std::unordered_map` | Rejected: grows with the number of addresses an attacker chooses, and allocates per new key |
| | A fixed table that forgets the least recently seen unpinned entry | Accepted |
| Error bodies | A small JSON code | Rejected: every status here already maps to one client action, and `Retry-After` carries the one number a client needs; a body is a second contract to keep stable |
| | Empty, with `WWW-Authenticate` on `401` | Accepted |
| Root with no `ULW_RUN_AS_USER` | Warn and stay root | Rejected in review: the process would serve as root on a warning nobody reads |
| | Refuse with exit 2 unless `ULW_ALLOW_ROOT=1` | Accepted, as ADR-0035 had it: the test harnesses that run as root say so |
| When to bind the port | After the drop, with everything else | Rejected: a port under 1024 could then not be bound at all |
| | Before the drop, handed to the server | Accepted; the descriptor limit is raised before it too |

## Decision

**Limits**, all per process, each a token bucket created full and refilled lazily on use:

| Limit | Default | Derivation | Refusal |
|---|---|---|---|
| Connections per address (direct), or unauthenticated requests in flight per forwarded address | 20 | Brief; a household behind one NAT, each browser opening up to 6 per origin and each uploader 3 `PATCH`es and a control request | Reset at accept; `429`, `Retry-After: 1` behind the proxy |
| New connections per address (direct only) | 10 a second, 10 saved | Brief; clients reuse connections, and each new one is a handshake | Reset at accept |
| Requests per user | 300 a minute, 300 saved | Brief; an uploader at 100 Mbit/s sends 8 MiB every 0.67 s, 90 `PATCH`es a minute however many run at once, which leaves 210 for playlists, polls and retries; caps one account at 300 x 8 MiB a minute, 40 MiB/s | `429`, `Retry-After` until one token |
| Upload bytes per user | 100 GiB a day, 100 GiB saved | Two 50 GiB uploads (the largest allowed) a day, one and a full retry; 1.7% of the 600 Mbit/s port's 6.5 TB a day; refilled at 1.2 MiB/s, so a refused 16 MiB `PATCH` waits 13 s | `429`, `Retry-After` until the `PATCH` fits |
| Concurrent uploads per user | 3 | Unchanged (ADR-0027) | `429`, `Retry-After: 5` |

The address checks run in `on_accept` before the socket is tuned, a slot is taken or TLS
starts. The per-user checks run once the token is verified; the byte check after the upload
slot is taken, which it gives back when it refuses.

**Tables.** Clients: 16,384 entries, about 1.2 MiB. A client's bucket is full again a second
after its last connection, and a full bucket is worth nothing, so the table only has to remember
the addresses of the last second: more new clients a second than one loop takes handshakes from.
Users: 16,384 entries, about 3.3 MiB; a user seen before the 16,384 most recent starts over with
full buckets. An entry with a connection or request open is pinned and never dropped, and the
client table always has one more entry than there can be connections. Open addressing with
linear probing and backward-shift deletion, over a hash seeded per process so that clients
cannot choose keys that collide. `rate_limit_entries` and `rate_limit_evictions_total` show both.

**Trusted proxies.** `ULW_TRUSTED_PROXIES` takes CIDR blocks, none by default, so a forged
header changes nothing unless an operator names Envoy; /0 is refused and a block shorter than /8
(IPv4) or /32 (IPv6) logs a warning. `ULW_TRUSTED_PROXY_HOPS` (default 1) says how many proxies
stand in front, and the client is that many entries from the right. The Askedin overlays set
K3s's default pod network, `10.42.0.0/16`, and one hop. The NetworkPolicy admits Envoy and the
`monitoring` namespace, both inside that block; a monitoring pod could name any address, which
only chooses the bucket its own unauthenticated requests go to. RUNBOOK step 1 checks the block
and that Envoy's Service keeps clients' addresses (`externalTrafficPolicy: Local`); step 5.4
checks, at debug level, that the gateway logs the tester's real public address. The sandbox
sets kind's `10.244.0.0/16` and keeps every limit at its default.

**Byte quota, best effort.** Each replica keeps it in the user table: it is counted per
replica, forgotten on a restart, and reset for a user evicted from the table.

**Errors.** Bodies stay empty. `401` carries `WWW-Authenticate: Bearer` when no token was sent,
and `Bearer error="invalid_token"` when one was refused (RFC 6750 section 3).

**Dropping root.** `os::drop_privileges` calls `setgroups(0)`, `setgid`, `setuid`, then sets
`PR_SET_NO_NEW_PRIVS`, and proves the result: the real, effective and saved ids are the target's,
no supplementary group or capability remains, no_new_privs reads back set, and `setuid(0)` and
`setgid(0)` fail. The user comes last because `setgroups` and `setgid` need the capability
`setuid` discards; no_new_privs because the runtime image ships setuid-root `su` and `mount`,
whose exec would otherwise hand root back without our ids showing it. The gateway and the
worker call it from `main` after the configuration is read and before any thread, socket or job
exists, when root, with `ULW_RUN_AS_USER`; so do `chat_server`, which faces the network as the
gateway does, and the `ulw_reaper` and `ulw_migrate` jobs, all through one helper,
`ops::leave_root`. The gateway and `chat_server` bind their listening sockets and raise
their descriptor limit first and hand the sockets to the server, so a port under 1024 works. Root
with no user named exits 2 unless `ULW_ALLOW_ROOT=1`. The TLS certificate and key are read
after the drop, at start and on SIGHUP, so they must be readable by that user. The tests need
root; CI runs them with `sudo`.

## Measurements

A local `gateway_server` on TLS over its own Postgres and MinIO, on the shared 4-core host, with
`tests/load`. The legitimate client is `legit_client.py`'s loop (an authenticated `GET` of its
video on a new TLS connection every 0.2 s, about 146 requests in 30 s) run as a process of its
own from 127.0.0.2; with the scripts' `--legit-url` it would share the attacker's process and
wait on its threads for Python's interpreter lock. The attack runs from 127.0.0.1 at `nice 10`,
since on a real network its own CPU is on its own machines, and is restarted until the 30 s are
up: slowloris dripping a header block a byte every 4 s over TLS, and the flood opening 500
connections a second, a third each unauthenticated, oversized-header and authenticated.

The tolerance: under attack the p99 may be at most twice the no-attack p99 of the same build.
A p99 of about 146 samples is the second slowest request, so a single scheduling hiccup moves it:
the no-attack p99 alone ranged from 8 to 24 ms over the eight clean runs, a factor of 3. Each
figure below is therefore the median of four runs, before and after interleaved; per-run p99s
follow in brackets. Runs during which the host's connection-tracking table was full (dmesg:
`nf_conntrack: table full, dropping packet`, which other work on the host also caused, and which
stalled connects for 1 to 7 s even with no attack running) are left out.

| Legitimate client | No attack | Slowloris, 50 | Slowloris, 500 | Flood, 500/s |
|---|---|---|---|---|
| Before (main 3bd696d): p99 ms | 11.9 [15.6 12.5 11.4 8.0] | 13.3 [10.5 13.5 20.5 13.1] | 18.0 [9.6 18.6 17.4 43.5] | 77.0 [54.5 57.9 97.7 96.1] |
| Before: errors | 0% | 0% | 59 to 63% | 0% |
| After: p99 ms | 15.7 [23.5 18.8 11.9 12.6] | 15.9 [14.2 17.6 11.2 18.3] | 12.4 [10.6 10.8 13.9 17.7] | 13.4 [12.6 10.0 14.3 41.8] |
| After: errors | 0% | 0% | 0% | 0% |

Latencies are of the requests that were answered. Before, a 500-connection slowloris from one
address took every connection slot and cost the legitimate client six requests in ten, and the
flood multiplied its p99 by 6.5, past the tolerance of 2. After, every median is within 1.1
times the no-attack p99 and no request failed. One flood run of four reached 41.8 ms, 3.3 times
its own no-attack p99; the flood before was above 54 ms in every run.

The attacks were refused where they are cheapest. In one pair of runs the flood's 30 s cost the
gateway 14,733 TLS handshakes before and 458 after, the other 14,739 connections reset at accept
(`connections_rejected_total{reason="ip_rate"}`); before, the 500-connection slowloris also had
197 connections closed for capacity, legitimate ones among them. The earlier method, with the
legitimate client inside the attacking process and the attack not niced, gave the same verdict
with larger numbers: flood p99 327 ms before and 13 to 70 ms after.

## Consequences

- One address or one account can no longer take the gateway from everyone else; many
  addresses still can, up to the process-wide limits. A distributed flood is for Envoy and what
  is in front of it; a distributed slowloris is still cut off at 10 s by the header timer.
- Clients behind one NAT share its 20 connections when they reach the gateway directly. Through
  Envoy only their unauthenticated requests share it; signed-in users are held to their own.
- Every limit is per replica: two replicas double what one user or address can reach.
- A wrong `ULW_TRUSTED_PROXIES` makes every client look like Envoy and refuses it past 20;
  `connections_rejected_total{reason="ip_connections"}` shows it at once. A wrong hop count
  counts clients as the wrong address, which RUNBOOK step 5.4 catches before prod.
- Load tools on one host that open connections directly need the per-address limits raised
  (tests/load/README.md), and the soak sets them. Test harnesses that run as root set
  `ULW_ALLOW_ROOT=1`.
- Reopen if the catalog port gains a way to refuse a claim for the user's limit: the concurrent
  upload limit could then hold across replicas at no extra round trip.

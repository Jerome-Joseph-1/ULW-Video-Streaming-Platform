# 0033. The node channel is framed TCP on the reactor

Status: Accepted
Date: 2026-09-29

## Context

A room has one owning node (ADR-0015). A member connected to any other node must reach the owner
with its mutations, and the owner must send each message it sequences to every node that has
members in the room. The nodes already share Postgres, which orders ownership: a claim raises the
room's `owner_generation`, and every owner write names the generation it holds. What is missing
is the path for the messages themselves, and a way for a node to find another's address: chat
runs as a Deployment, so replicas have neither stable names nor a fixed list of peers.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Postgres NOTIFY for fan-out, forwards as rows | No second channel; the database is already shared | Rejected: payloads are capped at 8000 bytes, below a 64 KiB message; every message would cross the database twice more; a listener that falls behind fills a queue shared by every session of the server |
| WebSocket between nodes | The codec exists | Rejected: masking, fragmentation and close handshakes buy nothing between our own processes, and the ADR-0029 decoder is the server side only |
| A broker (NATS, Redis) | Fan-out and reconnects for free | Rejected for the reasons ADR-0015 gives: one more stateful service, and it neither owns nor fences |
| Persistent TCP between nodes, length-prefixed frames, on the same reactor | What section 3 recommends; one connection per pair, no new dependency, bounded frames | Accepted |

## Decision

- A node dials an owner the first time it needs it and keeps one connection to it. Frames are a
  4-byte big-endian length, a type and the type's fields (`rt/src/wire.hpp`): `Hello`, `Proof`,
  `Subscribe`, `Unsubscribe` and `Send` go from the dialer to the owner; `Challenge`, `Reply`
  and `Deliver` come back on the same connection. A frame is at most 64 KiB of body plus 256 bytes, so a
  broken or hostile peer costs one frame of buffer before it is cut off.
- **Trust.** A node acts on the channel as its members' server: it names the sender of every
  message it forwards, and a subscription receives every body of a room. So only nodes may
  speak on it, and the port is bound to the node's own address (`ULW_NODE_ADDRESS`), never to
  every interface. Every node holds one secret, `ULW_NODE_SECRET` (at least 32 bytes, from the
  environment, never logged). Before anything else, the ends authenticate each other: the dialer
  sends `Hello` with a fresh 32-byte nonce; the owner answers `Challenge` with its node id, its
  own fresh nonce and HMAC-SHA256 over both nonces, both node ids and its role; the dialer
  checks that tag and the node id it dialled, then sends `Proof`, the same HMAC under the
  dialer's role. Tags are compared in constant time. A frame out of this order, a wrong tag or
  a handshake unfinished after 5 s closes the connection, and nothing the peer sent is acted
  on: no subscription, no send, no room created by a lookup. Fresh nonces on both sides make a
  recorded exchange worthless; the role in each tag keeps one side's tag from being reflected
  as the other's. Mutual TLS would also do, but it brings certificates to issue, rotate and
  check per pod, for a channel that only has to tell our nodes from everything else; a shared
  secret in the deployment's secret store is one value, rotated by rolling the Deployment.
  Payloads are not encrypted: the channel stays on the cluster's private network.
- A node with members in a room subscribes to the room's owner; the owner delivers each message
  it sequences to its own members and to every subscribed connection. A subscription lives as
  long as its connection. When the owner changes (a notification, a claim, a failed forward),
  the member node subscribes to the new one.
- Each node publishes its node-channel address, a numeric `host:port`, in `chat_nodes`, and finds
  others there by node id. Numeric only: resolving a name would block the loop.
- A node's name is held by one run at a time. Each run of `chat_server` picks a random
  incarnation and records it in `chat_nodes` when it advertises; its heartbeats keep it there.
  A second live process under the same name (a misconfigured `ULW_NODE_ID`) is refused until
  the first has been quiet for `kOwnerStaleAfter`, and meanwhile claims nothing and stays
  unready: two processes under one name would each take the other's rooms as their own,
  unfenced. A new run takes back, under a new generation, only rooms whose heartbeat predates
  its own start; a run claims nothing before it has advertised.
- Writes to one room are appended one at a time: the owner sends the next append only after the
  previous one returned, so fan-out leaves in `last_seq` order and a connection delivers in that
  order. Member nodes drop any delivery whose seq is not above the last one they delivered.

## Consequences

- The chat port for node traffic must be reachable only from chat pods: a NetworkPolicy
  admitting the `chat` pods alone to the node port is part of deploying chat. The handshake
  keeps other callers from doing anything, but not from trying.
- Anyone who has the secret can act as any user in any room; it belongs in the deployment's
  secret store beside the database credentials, and rotating it means restarting every node
  together (nodes with different secrets refuse each other).

Failure modes, and what each costs:

- **Owner dies or pauses mid-forward.** The forwarder waits for its Reply up to the owner's store
  timeout plus a second (3 s) and answers the client `unavailable`. It does not retry: the owner
  may have sequenced the message before it stopped. The client decides whether to send again.
  Meanwhile every node with members sweeps once a second and claims rooms whose heartbeat is
  older than 5 s; subscriptions move to the claimant.
- **Duplicate delivery.** A client that retries after `unavailable` can have its message
  sequenced twice, under two seqs. Detecting that needs a client message id, which the chat
  service (M17) adds. The node channel itself never repeats a message: a resubscription can
  replay nothing, since nothing is buffered, and a repeated seq is dropped at the member node.
- **Ordering.** Messages of one room reach every member in `last_seq` order, with gaps where a
  node was between owners or its connection dropped. Filling gaps is the resume protocol (M17),
  which reads history by seq.
- **A paused owner that resumes.** It still believes it owns its rooms. Its next owner write,
  whether a message's append or its once-a-second heartbeat, updates nothing; it stops acting
  as owner, answers the write's sender `fenced`, and delivers nothing. Notifications of the
  takeover only update where it routes; ownership ends at the fence, never before, so what a
  stale owner does is decided by the database and not by which event its loop saw first.
- An owner gives up a room that has had no members on it, no subscribed node and no write for a
  minute, so the rooms a node owns (and names in every heartbeat) follow what is in use; a node
  also takes at most 16384 rooms with members on it at once. The room's rows stay: creating
  rooms is limited per user at the client edge (ADR-0034), and deciding who may create one at
  all is membership's, in M17.
- Rows of departed nodes stay in `chat_nodes`; a restarted pod comes back under a new name.
  Reopen with a cleanup when the table is large enough to notice.
- One append per message per room caps a single room at about a thousand messages a second.
  Reopen (pipelined appends reordered by seq before fan-out) if a room needs more.

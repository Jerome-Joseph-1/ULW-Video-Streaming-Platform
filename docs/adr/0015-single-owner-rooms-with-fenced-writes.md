# 0015. One owning node per room, with generation-fenced writes

Status: Accepted
Date: 2026-09-28

## Context

chat_server runs as several replicas (ADR-0019), and a room's members may be connected to
different ones. A room needs one order for its messages, and MLS needs exactly one commit
accepted per epoch (ADR-0016), so something must serialize each room. Replicas can pause (CPU
steal on a VPS, a stopped process, a long page fault) and resume believing they still own what
they owned before.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Any replica writes; Postgres orders the room | No inter-node channel; replicas stay stateless | Rejected: ordering and MLS commit arbitration become row-lock contention on every message, and fan-out still needs a channel between replicas |
| A broker (Redis pub/sub, NATS) for fan-out and ordering | A common pattern with existing tooling | Rejected: another stateful service on two nodes, and pub/sub provides delivery, not ownership or fencing |
| One owner per room holding a lease, without fencing | The owner trusts its lease timer; simple | Rejected: a paused owner resumes inside what it believes is still its lease and writes alongside the new owner |
| One owner per room; non-owners forward; owner writes fenced on a generation | One sequencer per room; a stale owner is stopped at its next write | Accepted |

## Decision

- Each room has exactly one owning node. Non-owners forward mutations (sends, joins, leaves, MLS
  handshake messages) to the owner over the inter-node channel; the owner orders, persists and
  fans out.
- When the owner dies, the room is reassigned and its `owner_generation` increases
  monotonically.
- Every owner write is fenced on `(room_id, owner_generation)`: the statement matches only if the
  stored generation equals the one the owner holds. A former owner that was paused through a
  reassignment updates 0 rows, and on seeing 0 rows it stops acting as owner of that room.

## Consequences

- Fencing cannot recall messages already delivered. It stops a former owner at its next write;
  anything that owner sent to its connected clients before then stays delivered, even if the new
  owner's history does not contain it.
- A mutation that arrives at a non-owner costs one extra hop.
- During reassignment the room is briefly unavailable: forwarded mutations fail until the new
  owner is known, and clients retry.
- Load follows rooms: one busy room sits entirely on one node.
- Monitor fenced writes (each is a pause or split ownership that fencing caught), reassignments,
  and forwarding latency.
- Reopen if a single room's traffic outgrows what one node can sequence.

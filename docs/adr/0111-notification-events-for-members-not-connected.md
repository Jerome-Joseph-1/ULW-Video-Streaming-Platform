# 0111. Notification events for members who are not connected, as an opt-in hook

Status: Accepted
Date: 2026-10-15
Amends: ADR-0091 ("a member with no socket open hears nothing" holds only while the hook is
unset, still the default)

## Context

A call rings, and a message reaches, only the sockets a member has open (ADR-0056, ADR-0091).
Askedin wants Web Push notifications (browser notifications that arrive with the tab closed, on
Android, iPhone home-screen apps and desktop browsers) for calls and messages. It keeps the push
subscriptions, the VAPID keys and the notification text, and sends the pushes itself. What it
cannot know is when a push is needed: which of a member's devices have a socket open, when a
call starts ringing and how its ring ends. Only chat knows that, and no single node does: a
member's sockets can be on any node, a ring lives on the room's owner (ADR-0091), and a message
is sequenced by the room's owner for whichever node its sender is on.

Two facts constrain the design. A socket has no device id: only the `call` command names one.
And nothing in the cluster today knows which of a user's devices are connected, only whether the
user is, and only for a node that watches them (ADR-0056).

## Options

| Question | Option | Verdict |
|---|---|---|
| How events leave chat | A message queue (Kafka, NATS) | Rejected: a second system for an operator to run, for a few events a second |
| | HTTP POST to a URL the operator names, signed | Accepted: what Askedin asked for; the operator's backend already speaks HTTP |
| Delivery | Retry until acknowledged (at least once) | Rejected: a retry after a timeout may repeat a push the first request already sent, and a ring is over in 45 s; a late retry is worse than none |
| | One try, at most once, dropped and counted when the receiver is slow or down | Accepted |
| Which devices are connected | Ask every node at ring time over the node channel, and wait for answers | Rejected: no node knows how many nodes to wait for, and a message would pay the same wait |
| | Each node's events say what it delivered; the receiver aggregates | Rejected: puts a distributed join on every receiver, and still says nothing of a member connected nowhere |
| | A registry in Postgres: each node records the devices it has sockets for, under a lease it renews | Accepted: one statement answers "who is connected" for the ring's owner and for a message's node alike; a dead node's rows stop counting when its lease lapses |
| Coalescing messages | In memory on the sending node | Rejected: a member's room is written from many nodes, each its own coalescer |
| | A mark per (room, member) in the same statement that picks who is owed one | Accepted: one atomic statement, whichever node runs it |
| A socket's device | A command after connecting | Rejected: the socket is counted before it says, and a client that forgets is counted as nobody's |
| | A query parameter of the upgrade, `/rt?device=<uuid>` | Accepted: known before the socket is counted; optional |
| Declining from a notification | A new owner protocol | Rejected: the owner already takes a decline naming its user (CallSignalRequest) |
| | The service API asks the room's owner as a member would | Accepted |

## Decision

- **Off by default.** `ULW_NOTIFY_URL` (http or https) turns it on, with `ULW_NOTIFY_SECRET` (at
  least 32 bytes). Unset, chat writes nothing to the registry, sends nothing, and behaves exactly
  as before. `ULW_NOTIFY_MESSAGE_INTERVAL_SECONDS` (5 to 3600, 30 by default) is the message
  coalescing interval.
- **The registry.** Migration 0022 adds `notify_nodes` (a run of a node, by a random 64-bit tag,
  and when it last renewed) and `notify_sockets` (node, user, device; the nil UUID for a socket
  that named none). A node renews every 20 s; rows of a node not renewed within 60 s are not
  counted, and are deleted 10 minutes on. A node writes its changes in one statement at a time,
  as a set difference, so a socket that opens and closes before it is written costs nothing. A
  drain deletes the node's rows. The port is `core::ports::IConnectivityStore`, on Postgres
  (`infra/postgres/notify_store`) and in memory (`infra/notify/memory`), with one contract test
  for both.
- **Devices.** A socket names its device with `/rt?device=<uuid>`, the id the client already
  sends in `call` and links its push subscription to. A malformed one is refused `400`. A socket
  that names none still counts as the user's socket for messages; it is not a device a call event
  can list.
- **Call events** come from the room's owner, through a watcher of the ring (`IRingWatcher`): the
  first `call_ringing` of a call, with each callee's connected devices read from the registry,
  then `call_answered`, `call_declined`, `call_cancelled`, `call_missed` and `call_ended`. Not the
  ring's 15 s re-announcements, nor a group call's `call_left` and `call_moved`. A call's events
  leave its owner in order: one that comes while its `call_ringing` waits on the registry waits
  behind it. A registry that does not answer within 1 s sends `call_ringing` with `devices`
  `null` (unknown) rather than late.
- **Message events** come from the node whose client sent the message, once the owner has
  sequenced it: `message` names the room, the sender, the seq, and the members owed a
  notification: on the room's member list (a direct or group chat), not the sender, with no
  socket open anywhere, and not told of this room within the interval. Never content. A
  stream's live chat sends none.
- **The sender.** Each node queues at most 4096 events, POSTs them in batches of up to 64 as
  `{"events": [...]}`, one request at a time so a node's events arrive in the order it made
  them, each with a 2 s timeout. A full queue drops the new event; a failed or timed-out batch is
  dropped; both are counted. Nothing
  waits on the sender: a ring, a ticket and a message go on as if it were not there
  (`notify_events_total{outcome="dropped"|"failed"}` count what was lost). Each request
  carries `ULW-Signature: t=<unix seconds>,v1=<hex HMAC-SHA256 of "<t>.<body>" under the
  secret>`; each event carries an `id` (UUID) for the receiver to drop a repeat.
- **Answering and declining from a notification.** A socket opened after the ring started
  answers by call id (`call` with `answer`) as any other does; nothing changes. The service API
  gains `decline_call` (`room`, `call`, `user`: the owner's own decline, checked against the
  member list as from a socket) and `call_status` (`room`, `call`, optionally `user`: `ringing`,
  `answered` or `over`, for that user when named, as a group call may ring some while others
  talk), both asked of the room's owner by joining the room for the ask and leaving after. The
  status ask is a new ask kind (7) and two new outcomes; an owner of an earlier release reads it
  as malformed, and the API answers `503` until it is replaced.
- **Kubernetes.** `components/notifications` sets the three variables from `config.env` and
  chat's Secret, and adds a NetworkPolicy letting chat reach the receiver's port in its
  namespace: chat's own policy admits no in-cluster port of another namespace.

## Consequences

- With the hook set, every socket open and close is a row write (batched), and every node writes
  its lease three times a minute. A message in a direct or group chat costs one more statement,
  on the sending node, after it is sequenced.
- The registry is as fresh as its last write and its lease: a device whose socket closed a moment
  ago may still be listed connected, and a node that died is counted until its lease lapses (60 s).
  A push to a connected device, or none to one that just dropped, is possible at the edges; the
  receiver should treat `devices` as a hint for whom to spare, never as an assurance.
- At most once, best effort: a receiver that is down, slow, or unreachable during an owner change
  loses events, counted on the node that dropped them. A late `call_ringing` must be checked with
  `call_status` before it is shown.
- The receiver should be reachable only inside the cluster (a Service, and a NetworkPolicy that
  admits only chat): the events name users and devices.
- Reopen if a receiver needs events it cannot lose (then a durable outbox), or if presence learns
  devices (then the registry could follow it).

# 0019. Chat runs in its own binary

Status: Accepted
Date: 2026-09-28

## Context

gateway_server is shared-nothing: any replica can serve any request, and its memory is bounded by
the admission budget of ADR-0007 inside a 1 GB process. Chat is a different kind of service. It
holds long-lived WebSocket connections and room state shared across replicas, with one owner per
room and a channel between nodes (ADR-0015).

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Chat inside gateway_server | One binary and one deployment; the reactor, HTTP, TLS and JWT code are already there | Rejected: puts multi-node shared state into a shared-nothing service, WebSocket memory falls outside the upload budget, and a chat fault or restart drops uploads |
| An existing chat server, such as a Matrix homeserver | Mature, with clients already written | Rejected: brings its own user accounts and its own encryption scheme, both decided otherwise (ADR-0016, ADR-0018) |
| A separate binary, `chat_server` in `apps/chat` | Its own deployment, scaling and failure domain; shares libraries, not a process | Accepted |

## Decision

Chat runs in its own binary, `chat_server` (`apps/chat`), deployed as `chat` with 3 replicas. It
shares libraries with the gateway but not a process. gateway_server stays shared-nothing and
bounded to 1 GB.

## Consequences

- Two deployables to build, version, roll out and monitor, and shared libraries must suit both.
- 3 replicas on 2 nodes means one node runs two of them. Losing that node loses two thirds of chat
  capacity until its rooms are reassigned (ADR-0015), and with no PodDisruptionBudget a node drain
  can take both at once.
- Chat needs its own memory budget, derived from WebSocket and room state rather than from the
  upload pump.
- The front door routes chat's WebSocket paths to `chat` and the rest to the gateway.
- Reopen if chat's load stays small enough that a separate deployable costs more than the
  isolation it buys.

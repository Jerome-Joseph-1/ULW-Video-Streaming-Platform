# 0124. The service asks who is connected

Status: Accepted
Date: 2026-10-19

## Context

A backend deciding whether to push to a user could not ask chat whether that user had a socket
open (#222). The hook's events say who rang and who is owed a message, but a backend that pushes
for its own reasons (a mention, a reminder) had nothing to go on. Chat already records which
devices of which users are connected where, in the hook's registry (ADR-0111).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A service API `presence` that reads the hook's registry | One indexed read, of a table every node already writes; the same devices a ring's event names | Only with the hook on, since the registry is written only then | Accepted |
| Ask each node through the room plane, or through presence watches | Works with the hook off | A fan-out to every node, or a watch set up and torn down per question | Rejected |
| Write the registry whether or not the hook is on | `presence` always works | A write per socket open and close for deployments that never asked for it | Rejected |

## Decision

- `POST /service/v1/presence` takes `{users}`, 1 to 64 user ids. It answers, for each user
  once and in the order asked, `connected` and `devices` (the device ids their sockets named).
- It reads `IConnectivityStore::connected`, the read a ring's `call_ringing` uses, so a socket
  counts while its node's lease stands.
- With `ULW_NOTIFY_URL` unset there is no registry, and the answer is `409` `notify_off`.

## Consequences

- A backend checks presence in one call before it pushes.
- The answer is as fresh as the registry: a node writes its sockets' changes a quarter second
  after they happen, and a dead node's sockets count until its lease lapses (60 s).

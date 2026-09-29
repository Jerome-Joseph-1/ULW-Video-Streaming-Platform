# 0055. Chat soak flatness judged per command, delivery and connection

Status: Accepted
Date: 2026-09-29

## Context

The Definition of Done asks for a 6 h soak that shows flat RSS and descriptor counts for the
gateway and for chat. ADR-0042 defines flat for the gateway and the worker: the upper end of the
RSS slope's 95% confidence interval, per unit of work, must stay below what would carry the
process from its peak to its memory limit within 30 days at production's ceiling rate. Chat's
work is not requests, upload sessions or jobs, and its memory limit is not a systemd
`MemoryHigh`, so the criterion needs chat's own units, ceilings and limit, stated before a run
is judged.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| One unit, the client command | One number, what a client does | Rejected alone: a delivery is per subscriber, so a leak there would be read against whatever fan-out the soak happened to use |
| Commands, deliveries and connections, each at its own ceiling | Each is a path that allocates per event: the parsed command, the frame per subscriber, the session and its rooms | Accepted |
| The margin between the 1 GiB pod and the 730 MiB worst case (ADR-0043) as the memory left | A leak should not eat what admission already promised | Rejected: 294 MiB gives bounds of a few kilobytes an hour at soak load, under one page of RSS over the run, which a flat process cannot show either |
| Deliveries at the node's 540 Mbit/s (ADR-0012) | A physical ceiling | Rejected: about 66,000 a second, a bound no soak can resolve, and the send limits cap the traffic long before the link does |

## Decision

`tests/soak/chat_soak.py` runs three `chat_server` nodes on one Postgres, as the M16
acceptance does, under the load its docstring lists, and judges each node by ADR-0042's method:
15 minutes of warm-up excluded, a least-squares line over the rest, the 95% upper end judged, and
a run too short or too quiet to resolve a bound fails.

- The memory limit is the chat pod's 1 GiB (ADR-0036, ADR-0043); chat has no unit file and
  so no lower `MemoryHigh`.
- Ceilings per node come from the limits the service enforces: 1280 connections, each its own
  user, at the sustained 2 sends and 1 join or history page a second.
  - Commands: 1280 x 3 = 3840 a second; about 0.10 bytes a command.
  - Deliveries: those 2560 sends each delivered to ten members, ADR-0012's larger room:
    25,600 a second; about 0.016 bytes a delivery.
  - Connections: all 1280 reconnecting every 30 s, the linger the resume path is sized for:
    42.7 a second; about 9.5 bytes a connection.
- Descriptors: the fitted rise over the window must stay below the connections the soak can
  hold on one node at once (every client, the visitors in flight, the slow consumers, the burst
  connection, the firehose and a refused upgrade), or the warm-up's range if larger.
- The run must also have exercised every path of its mix (owner takeovers and fenced writes,
  rate limits, deduplication, resumes, lossy skips, slow consumers, refused upgrades, bad
  commands, SIGHUP, and history and presence where the server has them), and every node must
  exit 0 on SIGTERM.

## Consequences

- At the soak's default load (64 clients, about 90,000 commands, a million deliveries and 2,000
  connections an hour per node) the bounds need the RSS slope's upper end under roughly 10 to 20
  KB an hour: six hours of a process that does not grow, not two.
- A live chat whose viewers far outnumber ten members fans out more per send than the delivery
  ceiling assumes; its soak belongs with M32's live chat.
- Not driven: node restarts (a new process starts a new series; SIGSTOP drives the ownership
  changes instead), the JWKS fetch path and database outages.

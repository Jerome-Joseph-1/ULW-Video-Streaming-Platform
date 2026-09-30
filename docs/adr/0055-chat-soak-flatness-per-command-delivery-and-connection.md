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

A first 45-minute run (main at 48af100, io_uring, no prefill) showed RSS still rising well after
the warm-up, in MiB:

| minute | 0 | 5 | 10 | 15 | 20 | 25 | 30 | 35 | 40 | 44 |
|---|---|---|---|---|---|---|---|---|---|---|
| chat-1 | 18.0 | 30.0 | 32.6 | 36.1 | 37.9 | 38.9 | 39.7 | 40.7 | 41.2 | 42.5 |
| chat-2 | 18.5 | 26.7 | 30.3 | 32.6 | 32.8 | 35.0 | 36.6 | 38.5 | 39.6 | 40.6 |
| chat-3 | 18.0 | 29.1 | 31.2 | 31.7 | 33.8 | 34.8 | 38.1 | 39.7 | 40.4 | 40.6 |

The fit after minute 15 put the slopes' upper ends at 13 to 23 MB an hour: 143 to 248 bytes a
command against 0.10. No huge pages were involved (`AnonHugePages` stayed 0), and the growth
was two structures that are bounded but reach their bound later than the warm-up ends:

- io_uring's receive pool, 256 buffers of 64 KiB (16 MiB) per reactor, allocated without being
  touched. A buffer's pages count in RSS once the kernel has written into them, so the pool's
  RSS is the sum of the largest receive each buffer has ever held, and the rare large receive
  (a burst, a resume, a batch on the node channel) keeps adding to it for hours.
- The chat service's order of kept messages (`kept_order_`), capped at 131,072 entries of 24
  bytes, about 3 MB. Its entries include messages a room has already dropped on its own 256 KiB
  limit, so it always reaches the cap; at the soak's ~60 messages a second into each node that
  takes about 35 minutes (116,000 entries measured at minute 31).

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| One unit, the client command | One number, what a client does | Rejected alone: a delivery is per subscriber, so a leak there would be read against whatever fan-out the soak happened to use |
| Commands, deliveries and connections, each at its own ceiling | Each is a path that allocates per event: the parsed command, the frame per subscriber, the session and its rooms | Accepted |
| The margin between the 1 GiB pod and the 730 MiB worst case (ADR-0043) as the memory left | A leak should not eat what admission already promised | Rejected: 294 MiB gives bounds of a few kilobytes an hour at soak load, under one page of RSS over the run, which a flat process cannot show either |
| Deliveries at the node's 540 Mbit/s (ADR-0012) | A physical ceiling | Rejected: about 66,000 a second, a bound no soak can resolve, and the send limits cap the traffic long before the link does |
| A longer warm-up | No new load | Rejected: the pool's last pages arrive with rare large receives, whenever those happen; no fixed length is known to cover them |
| Pre-touching the pool in the reactor | RSS would hold the whole 16 MiB from the start, in the soak and in production alike | Left to the reactor's owners: it trades 16 MiB committed per reactor from the start for a flat line, against the reactor's deliberate choice to cost idle connections nothing |
| A prefill in the warm-up that drives both structures to their bounds | Only the soak changes; what is judged after the warm-up is the steady state production reaches after a while | Accepted |

## Decision

`tests/soak/chat_soak.py` runs three `chat_server` nodes on one Postgres, as the M16
acceptance does, under the load its docstring lists, and judges each node by ADR-0042's method:
15 minutes of warm-up excluded, a least-squares line over the rest, the 95% upper end judged, and
a run too short or too quiet to resolve a bound fails.

- The warm-up does work of its own before the window opens, stopping two minutes before it
  ends:
  - 2,048 frames of 60 KiB that are not JSON to each node, over loopback's 64 KiB MSS, in
    writes of eight (480 KiB): the socket holds more than a buffer whenever the node reads, so
    every read but a write's last fills a whole 64 KiB buffer, and the ring goes round more than
    seven times. They are refused as `not_json` and never sequenced or stored. Sent one at a
    time and answered between, as a first version did, reads found less than a buffer waiting
    and the pool still gained 0.2 to 0.4 MB after the warm-up; batched, its mapping held 18,028
    KiB on every node from the first minute and did not move (the rest of the mapping is the
    datagram ring, which TCP never uses).
  - 60 more senders in the soak's pool rooms, two messages a second each (the send limit),
    with bodies drawn as the clients' are: together with the soak's own traffic, each node's
    kept-message order reaches 131,072 entries within the warm-up. The pool rooms are at their
    own 256 KiB by then, so nothing more is kept than the load keeps. A first version gave the
    prefill rooms of its own; they held about 15 MB of kept messages that the order's cap then
    dropped during the window, freeing heap that growth elsewhere would have filled unseen in
    RSS, so it was not used.
  The soak runs the allocator as production does: no glibc tunable is set on the nodes.
  This loosens nothing: the bounds, the fit and the window are unchanged, and both structures
  would reach the same size in production within hours of a start. After it, the pool's RSS
  cannot grow and the order stays at its cap, so a leak shows as it did before: a slope whose
  upper end stays above the bound per command, per delivery or per connection, for the whole
  window.
- Each node starts with transparent huge pages disabled for it (`PR_SET_THP_DISABLE`, which
  holds across exec), as the datagram soak does since #50: where they are "always", a collapse
  adds up to 2 MiB to RSS with nothing allocated.
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
  commands, SIGHUP, history and presence where the server has them, and the prefill), and every
  node must exit 0 on SIGTERM.

## Consequences

- At the soak's default load (64 clients, about 90,000 commands, a million deliveries and 2,000
  connections an hour per node) the bounds need the RSS slope's upper end under roughly 10 to 20
  KB an hour: six hours of a process that does not grow, not two.
- A live chat whose viewers far outnumber ten members fans out more per send than the delivery
  ceiling assumes; its soak belongs with M32's live chat.
- Not driven: node restarts (a new process starts a new series; SIGSTOP drives the ownership
  changes instead), the JWKS fetch path and database outages.

## What the prefill left, measured

Runs of 40 minutes on M32 (fe1cbca and 965badd), which includes M18 and M19, with the prefill,
found no allocation that grows without a bound, and three that each finish after the warm-up.
heaptrack attached to one node at minute 17 showed the live heap of new allocations level at 14
to 15 MB after three minutes and nothing leaked at exit; ChatService's maps (clients, rooms, the
per-user buckets, whose bucket counts stayed at 59), the router's remembered keys (about 3,400,
a minute's worth) and its room maps held steady between minutes 17 and 38.

1. **The order of kept messages.** Since M32's fix of stale entries (they no longer count
   against the kept messages), the order is compacted only when it holds twice the bound,
   262,144 entries (about 6.3 MB): it went from 150,000 to 221,000 entries on every node between
   minutes 17 and 38. Fix, in M32: compact at 1.25 times the bound (about 164,000 entries,
   3.9 MB), which the warm-up reaches.
2. **Heap pages never written until after the warm-up.** Each session reserves 256 KiB for
   parsing its upgrade request and keeps it for the connection's life. glibc raises its mmap
   threshold when a large mapped chunk is freed, so these reservations come from the brk heap.
   The heap grows to the peak of concurrent sessions times 256 KiB, mostly never written, and
   RSS creeps into it as later allocations land there. A trace of one node (brk, mmap, munmap,
   madvise) showed the heap only growing, no call at all from minute 12.5 to shutdown, and no
   munmap while about 1,000 sessions came and went; so nothing was trimmed and faulted back.
   `[heap]` from `/proc/<pid>/smaps` (Rss, Private_Dirty and Anonymous equal throughout), in MB:

   | node | size, min 17 and 38 | Rss min 17 | Rss min 38 | never written, min 38 |
   |---|---|---|---|---|
   | chat-1 | 29.4 | 20.9 | 21.1 | 8.3 |
   | chat-2 | 29.3 | 17.5 | 18.8 | 10.5 |
   | chat-3 | 27.5 | 17.1 | 18.8 | 8.7 |

   RSS went from 51.5, 48.5 and 47.3 MiB at minute 15 to 52.2, 50.5 and 49.6 MiB at minute 40;
   the slopes' upper ends were +1.7, +5.6 and +6.5 MB an hour. As a diagnostic only, the same
   run with the threshold fixed (`GLIBC_TUNABLES=glibc.malloc.mmap_threshold=131072`), so the
   reservations stay mapped and are unmapped with their session:

   | node | heap size | Rss min 17 | Rss min 38 | never written | RSS min 15 / 20 / 40 (MiB) |
   |---|---|---|---|---|---|
   | chat-1 | 18.4 | 18.22 | 18.22 | 0.2 | 49.9 / 50.0 / 50.0 |
   | chat-2 | 16.1 | 15.90 | 15.90 | 0.2 | 47.0 / 47.1 / 47.3 |
   | chat-3 | 16.0 | 15.46 | 15.76 | 0.2 | 47.0 / 47.1 / 47.4 |

   with upper ends of +0.23, +1.5 and +1.5 MB an hour. Fix, in the product: release the
   upgrade parser once the handshake is done, and reserve its buffer as input arrives. The
   256 KiB per connection was also missing from ADR-0036's budget.
3. **The receive pool's last pages**, left by prefill frames sent one at a time: fixed in the
   soak by writing them in batches (above).

The next verification runs once the parser fix and M32's compaction are on `main`. What remains
after those three is the steady state: a slope whose upper end stays above a bound per command,
per delivery or per connection for the whole window is a leak.

# Soak runs

`tests/soak/soak.py` runs `gateway_server` and `transcode_worker` against the local Postgres and
MinIO for hours, samples each process's RSS and open descriptors every minute into
`samples.csv`, and judges whether both stayed flat. The criterion, and why, is ADR-0042.

```
docker compose -f deploy/local/compose.yaml up -d --wait
cmake --preset ci && cmake --build --preset ci
tests/soak/soak.py --build build/ci --hours 6 --out /tmp/soak-6h          # a run
tests/soak/soak.py --rejudge /tmp/soak-6h/samples.csv --clients 8        # judge it again
```

The soak sets `ULW_ALLOW_ROOT=1` for the processes it starts, since a development host may run it
as root, and both services refuse root otherwise (ADR-0052). Started by hand as root, either
service needs `ULW_ALLOW_ROOT=1`, or `ULW_RUN_AS_USER` naming the user to become.

`--rejudge` prints two verdicts: the per-unit criterion in force (the RSS slope's 95% upper end
per request, per chunk request, per upload session and per job, against production ceilings),
and the per-hour criterion it replaced. Pass `--clients` as the run used it; it sets the
descriptor bound.

The job ceiling is 1.67 a second (the clip's 0.6 s end to end, the least a job takes), which
bounds the worker at about 413 bytes a job; the runs below were judged again with it and their
verdicts did not change. Chunk requests are counted from the soak that follows these runs; the
three runs here predate the column, so for them `--rejudge` reports the per-chunk line as not
judged, and a leak on each chunk shows only through the per-session line, since every chunk
belongs to a session.

### What the summary says beyond the verdict

Before the load starts, one clip is uploaded and must come back ready, or the run stops with
exit 2 and the worker's reason. Without ready videos the playlist load turns into bad requests
and the store faults never delete anything, as in runs 37079940150 and 37108394263, whose every
probe failed: the soak ran as root with `--out` under the runner's home directory (mode 750,
another owner), and the sandbox drops every capability before ffprobe opens the source, so root
could not reach it. The soak now puts the worker's scratch in a fresh directory under `/tmp`
when `--out` cannot be reached that way.

After the verdict, and from `--rejudge`, the summary prints, for information only:
- the gateway's RSS at 0, 15, 30, 45 and 60 minutes, then every hour, with the change between
  marks, and the fitted slope over the last 4, 2 and 1 h (with its upper end per request),
  which tells a plateau from a steady tail;
- each load path's count (videos made ready or failed, with the reasons, playlists fetched,
  store faults that reached the fetch, commits, resumes, cancellations, saturations refused by
  the gateway's limit, slow clients ended by a timer), marked when one never ran;
- every 5xx: the clients' by the action that got it, and the gateway's, from `gateway.log`, by
  route and status, marked when it fell inside a saturation or a store fault. Both make 5xx by
  design: while the saturation holds every upload slot, any chunk request, the soak's own
  uploads and resumes included, gets 503; a ready video whose media playlist is missing from
  the store gets 500. A 5xx outside both is listed with its time.

A run in which any of those load paths (all but failed videos) never ran is reported
`INVALID: load path never exercised: ...` below its verdict and exits 2: it did not apply the
load it names, so its verdict on RSS and descriptors, printed unchanged, is not evidence either
way. At cleanup the soak empties and deletes its MinIO bucket, as it drops its database.

## On a runner

`.github/workflows/soak-experiment.yml` runs either soak, or both (on two hosted runners at once, or one after the
other on the self-hosted one), at any ref. Dispatch it from the Actions tab, or with
`gh workflow run soak-experiment.yml --ref <branch> -f runner=self-hosted -f kind=chat -f hours=6`;
`runner` is `hosted` (GitHub's) or `self-hosted` (the project's own host), `kind` is `chat`,
`gateway` or `both`, `hours` from 0.5 to 4.9 on a hosted runner and to 22 on the self-hosted
one (its job token lives 24 h), and `clients` empty for the scripts' own (64 and 8) or up to 1024. It builds the `ci`
preset at that ref, runs against a Postgres on the same image as `deploy/local/compose.yaml` and
that file's MinIO, and uploads `samples.csv`, the summary and the logs as an artifact whatever
the verdict, for `--rejudge` or a closer look. A hosted job ends at 6 h, build included, so the
6 h acceptance soak runs on the self-hosted runner.

The soak runs as root on either: the services are undumpable (`ops::disable_core_dumps`), so
only root reads the `/proc/<pid>/fd` it samples. The self-hosted runner is a dedicated Ubuntu
24.04 host whose runner user has Docker and password-less sudo; the job installs what a hosted
image has and the host lacks (cmake, docker compose, rustup 1.29.1 by digest), and leaves
nothing root-owned in its workspace and no service running. The runner user's sudo rule must be
`ALL`, since the job passes environment to sudo. The setup action's two sysctls (unprivileged
user namespaces allowed, 28 bits of mmap randomisation) stay set on that host until it reboots;
it runs nothing else. Postgres is published on loopback only: a published port would bypass the
host's firewall. Anyone who can dispatch a workflow at any ref runs that ref's code as root on
the host, so it holds nothing else; one soak runs there at a time. The workflow's concurrency
group is per runner kind, so it also serialises hosted runs: a second dispatch for the same kind
waits for the first, and a third replaces the one still pending, which is then cancelled.

## Runs

The first two runs used the load and criterion of the soak's first version: four clients on
plain HTTP at about 5 requests a second (playlists, bad requests, uploads, resumes,
cancellations), and RSS judged by its slope per hour. Both are re-judged below under the
current criterion. Their binaries were built from 6859bb9, the branch before its rebase onto
d33539e; the rebase changed no code the soak exercises (it merged the worker's heartbeat file).

### 2 h, 2026-09-29 04:40 to 06:46 UTC

A two-hour run, recorded as one. 126 samples; judged window 111 samples over 1.83 h.
35,266 2xx, 3,391 4xx, no 5xx, no transport errors: 586 uploads committed,
1,181 resumes, 1,210 cancellations, 8,114 playlists, 3,883 bad requests. Both processes exited
0 on SIGTERM.

| | first | last | peak | slope | 95% upper end |
|---|---|---|---|---|---|
| gateway RSS | 36.5 MiB | 36.5 MiB | 36.6 MiB | +12.2 KB/h | +52.3 KB/h |
| worker RSS | 14.8 MiB | 14.8 MiB | 14.8 MiB | 0 | 0 |
| gateway fds | 25 | 25 | 25 (min 23) | -0.07/h | rise at most +0.29 |
| worker fds | 4 | 4 | 9 (min 4) | +0.07/h | rise at most +0.66 |

- Per-hour criterion (replaced): **pass**. Gateway 0.012 MB/h against 0.780; worker 0 against
  2.478; descriptors flat.
- Per-unit criterion (current): **fail, not resolved**. At 18,507 requests an hour the gateway's
  upper end is 2.83 bytes a request against a bound of 0.32: two hours at five requests a
  second cannot resolve it. Per upload session 37.1 bytes against 40.9, flat; worker 0 bytes a
  job against 413, flat; descriptors flat.

### 6 h, 2026-09-29 04:40 to 10:40 UTC

Started beside the 2 h run on the same binaries and load. 360 samples; judged window 345
samples over 5.73 h. 99,947 2xx, 9,207 4xx, 10 5xx, no transport errors: 1,723 uploads
committed, 3,363 resumes, 3,364 cancellations, 22,952 playlists, 10,824 bad
requests. Both processes exited 0 on SIGTERM.

The ten 5xx were all between 10:12:43 and 10:12:55 UTC, while the shared Postgres container had
been left paused by another suite's test (`docker pause`) and was then resumed; they are that
outage answered as 503, not the soak's doing.

`videos_ready` in the samples stays at 200 from minute 118: it is the size of the working set
of ready videos the clients play from, capped at 200 by design, not a count of what the worker
finished. `uploads_committed` (1,723) counts the commits.

| | first | last | peak | slope | 95% upper end |
|---|---|---|---|---|---|
| gateway RSS | 36.5 MiB | 36.1 MiB | 37.0 MiB | -83.2 KB/h | -67.9 KB/h |
| worker RSS | 14.8 MiB | 13.9 MiB | 14.8 MiB | -168.4 KB/h | -154.2 KB/h |
| gateway fds | 25 | 25 | 29 (min 22) | +0.06/h | rise at most +0.69 |
| worker fds | 4 | 4 | 9 (min 4) | +0.08/h | rise at most +0.89 |

- Per-hour criterion (replaced): **pass**.
- Per-unit criterion (current): **pass**. The upper ends are negative: RSS fell slightly over the
  window. Gateway at most -3.7 bytes a request (bound 0.32) and -48 a session (bound 40.9);
  worker at most -536 bytes a job (bound 413); descriptors flat against bounds of 27 and 16.

This run lacked the paths the current load adds: TLS, the header, body idle and body rate
timers, full admission limits, store faults and certificate reloads.

### 6 h on the current load, started 2026-09-29 10:41 UTC

Binaries from ee075b6, the current soak load (eight clients on TLS, about 30 requests a second,
with slow clients, saturation, store faults and SIGHUPs) and criterion.

Run 3 (full load) in progress; recorded when it ends at about 16:41 UTC.

### 6 h with the gateway's malloc tuned (ADR-0094), two runs

Both on the self-hosted runner (`soak-experiment`, `kind=gateway`), with the gateway calling
`mallopt` for one arena and a fixed 128 KiB mmap and trim threshold at startup. On main before
it, the gateway's steady RSS was about 30.9 MB and it crept by +0.53 MB after the warm-up.
Per-unit values are the RSS slope's 95% upper end, against bounds of 0.315 bytes a request,
0.315 a chunk request and 39.85 an upload session.

| | run 37146904045 | run 37168049376 |
|---|---|---|
| Branch, commit | exp/soak-allocator, 6163f0d | demo/integration with #145, 3b138a3 |
| Started (UTC) | 2026-10-03 19:11 | 2026-10-04 01:38 |
| Gateway RSS, first / last | 49.6 / 50.0 MiB | 50.0 / 50.5 MiB |
| Creep after the warm-up | +0.42 MiB | +0.5 MiB |
| Slope | +48.9 KB/h (95% upper end +52.5) | +105.1 KB/h over the window, +5.4 KB/h over the last hour |
| Per request | 0.379 B | 0.775 B |
| Per chunk request | 8.23 B | 16.8 B |
| Per upload session | 45.2 B | 92.4 B |
| Verdict | fail | fail |

In both, the worker's RSS and both processes' descriptors were flat, the run was not INVALID,
and every 5xx was injected by the soak. The tuning raised the steady RSS by about 19 MB and
barely changed the creep, which levels off late in each run; it was not adopted (ADR-0094).

## Chat

`tests/soak/chat_soak.py` runs three `chat_server` nodes on one Postgres, the M16 cluster, for
hours under a mixed WebSocket load through all three, samples each node's RSS, open descriptors
and `/metrics` every minute into `samples.csv`, and judges every node by ADR-0042's method on
chat's units of work: per command, per delivery and per connection, against ceilings of 3840,
25,600 and 42.7 a second per node and the pod's 1 GiB. The units, ceilings and why are
ADR-0053; the load is listed in the script's docstring. It needs Postgres only, and sets
`ULW_ALLOW_ROOT=1` for what it starts, as the gateway soak does.

```
docker compose -f deploy/local/compose.yaml up -d --wait postgres
cmake --preset ci && cmake --build --preset ci --target chat_server ulw_migrate ulw_devtoken
tests/soak/chat_soak.py --self-test --out /tmp/chat-soak-selftest            # 12 minutes
tests/soak/chat_soak.py --build build/ci --hours 6 --out /tmp/chat-soak-6h    # the run
tests/soak/chat_soak.py --rejudge /tmp/chat-soak-6h/samples.csv --clients 64
```

`ULW_TEST_DATABASE_URL` names the Postgres server (the compose one by default); the run creates
its own database there and drops it at the end. Client ports are 19101 to 19103 and node ports
100 above (`--port`). `ULW_REACTOR=epoll` runs the nodes on the fallback reactor.

- **The mix follows the server.** Where the database has member lists (M19), the soak lists
  its users as members of the rooms they use, as an operator would. At the start it tries
  `history` and `watch`; a `malformed` answer means that server predates them (M19, M18), and
  the summary names what it left out. Run the 6 h soak on a `main` that has M18 and M19, so
  that history pages, stored messages and presence are part of what is judged.
- **Verdict.** A run passes when every node is flat, every path of the mix ran (the summary's
  "paths exercised" list: owner takeovers and fenced writes, rate limits, deduplication,
  resumes, lossy skips, slow consumers, refused upgrades, bad commands, SIGHUP, and history and
  presence where present), nothing in the soak itself raised, and every node exited 0 on
  SIGTERM.
- **Warm-up prefill.** The first 13 minutes also fill io_uring's receive pool and each node's
  order of kept messages to their fixed sizes, which the load alone reaches only after the
  warm-up; the nodes run without transparent huge pages. Why, and what a leak still looks like
  after it, is ADR-0069. The summary's "warm-up prefill ran" says it did.
- **Self-test.** 12 minutes, 15 s samples, a 3 minute warm-up, owner changes every 150 s and
  shorter client sessions. It proves the harness: it passes on coverage and clean exits, and
  prints its flatness without judging by it, since nine minutes cannot resolve bounds set for
  six hours. `tests/soak/chat_soak_test.py` (ctest `chat_soak_judge`) checks the verdict on
  synthetic samples.
- **Storage.** On a server that stores messages (M19) 6 h leave about 1.5 million of them, some
  0.8 GB of rows with the slow consumers' firehose, until the database is dropped at the end.
- **Owner changes stall clients.** Each stop holds one node for 8 s: its clients' sends go
  unanswered, some connects to it time out, and those count as `transport_errors` in the
  totals, not as failures.

### Runs

No 6 h run yet; it belongs on a `main` with M18 and M19, and is recorded here when it ends.

#### 45 min, 2026-09-29 21:20 to 22:05 UTC, with the prefill

Main at 76a8a17 (without M18 and M19), io_uring, 64 clients; conntrack stayed under 7,500
entries. Every path of the mix ran, and every node exited 0 on SIGTERM. 30 samples judged, over
0.48 h after the warm-up. RSS in MiB:

| minute | 0 | 5 | 10 | 15 | 20 | 25 | 30 | 35 | 40 | 44 |
|---|---|---|---|---|---|---|---|---|---|---|
| chat-1 | 17.7 | 46.3 | 49.2 | 51.3 | 52.0 | 52.4 | 52.3 | 52.4 | 52.7 | 53.0 |
| chat-2 | 18.7 | 45.6 | 47.7 | 48.6 | 48.1 | 48.8 | 49.0 | 49.6 | 50.0 | 50.1 |
| chat-3 | 19.0 | 46.0 | 48.0 | 48.6 | 49.1 | 49.2 | 49.5 | 49.5 | 49.5 | 49.5 |

- The receive pool held 17.9 MB from minute 5 on, and each node's kept-message order was at
  131,072 entries by minute 30 (read from the processes).
- The slopes' upper ends were +3.2, +5.5 and +2.2 MB an hour, against 13 to 23 before the
  prefill: 33, 66 and 23 bytes a command against 0.10. **Not shown flat.** Half an hour cannot
  resolve a bound of about 9 KB an hour, and the heap still rose by 0.5 to 1.4 MB between
  minutes 15 and 40, most on chat-2; the 6 h run decides whether that settles.
- An epoll run beside it (30 min, 15 samples judged) was alike: upper ends of +2.1 to +6.7 MB
  an hour, heap +0.4 to +1.1 MB after the warm-up and slowing.

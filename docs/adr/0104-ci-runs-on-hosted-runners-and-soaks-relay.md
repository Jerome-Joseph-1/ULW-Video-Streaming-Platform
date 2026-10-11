# 0104. CI runs on hosted runners only, and a soak longer than a hosted job runs as a relay of segments judged together

Status: Accepted
Date: 2026-10-11

## Context

Since #121 the jobs that need a recent io_uring (`build-test`, `tsan`, `reactor-matrix`,
`autobahn`, `fuzz`) ran on one self-hosted machine with four runners (`vmi3627724`), and the
6 h acceptance soak ran there because a hosted job ends at 6 h, build included
(docs/operations/soak.md). The machine had 6 vCPUs, 12 GB and no swap. On 2026-10-09/10 its
queue reached about 45 jobs, a clang-tidy shard hit its limit, main's run started none of its
jobs for five hours and publish-images gave up twice (docs/operations/ci-lanes.md). The
machine is being removed. Nothing in production depends on it.

Two things followed from removing it:

- **Every job runs on a hosted runner.** The repository is public, so hosted minutes are free.
  The io_uring jobs depend on the hosted kernel. `reactor-matrix` runs
  `tools/io_uring_probe.c` first, so a kernel that cannot serve the reactor is named as the
  cause and its tests don't fail one by one.
- **A soak can't run in one process for longer than one hosted job.** A job lasts at most
  360 min. Building takes 15-30 of those, and setup takes more.

ADR-0042's criterion uses only the samples after a 15 min warm-up. It fits the RSS per hour by
least squares and takes the 95% upper end of the slope. It divides that by each unit of work
done per hour, and compares the result with what would reach `MemoryHigh` in 30 days at the
production rate. A leak shows as bytes added per unit of work. The process's age doesn't
matter: what counts is how much work it has done.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Keep a self-hosted machine for soaks only | One process for 6 h or 22 h, as before | A machine to keep patched, its kernel, limits and sudo set up (soak.md listed eight requirements), for a run made a few times a release; anyone who can push runs code as root on it | Rejected |
| Shorten the acceptance soak to what one hosted job holds (about 5 h) | Nothing new | Five hours of work after the warm-up, fixed; no way to collect more evidence | Rejected as the only choice; kept as the one-segment case |
| Checkpoint the process and restore it in the next job (CRIU) | One logical process across jobs | Root, kernel support and restoring sockets, io_uring rings and Postgres connections; a restored process isn't the one that ran | Rejected |
| Run the load against a server outside GitHub | One process for any length | A server to run and pay for, which is the machine being removed | Rejected |
| **A relay:** build once, run N segments of at most 5.25 h each on runners of their own, and judge all their samples with one fit that has a shared slope and an intercept per segment | Any total length, up to 60 h; segments run in parallel, so 24 h of load takes about 5-11 h of wall time; one segment gives exactly the old verdict | No process lives longer than one segment; each segment pays its own 15 min warm-up | Accepted |

## Decision

**Runners.** Every job in every workflow runs on `ubuntu-24.04`, whatever the event. ci.yml's
`trusted-runner` anchor, the `clear-runner` step, `ULW_SHARED_RUNNERS`, `restart-runner.yml`,
`host-diag.yml` and the setup action's self-hosted branches (apt retries and locks, installing
what the hosted image has, the io_uring probe, the kept caches, the rustup lock) are removed.
The setup action is back to what it was before #121.

**The relay** (`soak-experiment.yml`):

1. `plan` validates the inputs and splits `hours` (0.5-60) into the fewest segments of at most
   `segment-hours` (0.5-5.25), all of the same length rounded up to the minute. A segment
   shorter than 30 min is refused.
2. `build` builds the targets once and uploads them as a tarball, which keeps the executable
   bits an artifact would drop.
3. `segment` (a matrix of kind × segment, at most `parallel` at a time) starts Postgres and,
   for the gateway, MinIO. It unpacks the binaries and runs the soak script unchanged for the
   segment's hours. Exit 1 (not flat) is that segment's own result and is kept for the judge.
   Any other non-zero exit fails the job.
4. `judge` (one per kind) downloads every segment's artifact and runs
   `<script> --rejudge seg1/samples.csv seg2/samples.csv ...`. Its exit status is the verdict:
   0 flat, 1 not flat, 2 no evidence either way. It also fails if a segment is missing.

**The fit.** `soak.fit_within(series)` is least squares with a shared slope and one intercept
per series. Each segment is centred on its own means; the slope is
Σ(x−x̄ᵢ)(y−ȳᵢ) / Σ(x−x̄ᵢ)²; the residual degrees of freedom are n − k − 1. A fresh process
starts at whatever level its allocator puts it, and the intercepts absorb that. A leak adds the
same bytes per unit of work in every segment, and the shared slope measures it. With one series
this is the plain fit, so `fit` is `fit_within([points])` and a one-segment run is judged as
before. The rate of each unit of work is the sum of the segments' rises over the sum of their
hours. The descriptor rise is the pooled slope's upper end times the longest segment's window.
That is compared with the larger of what can be in flight and the widest swing seen in any one
segment's warm-up.

**Soundness.** Each run now writes `result.json`, `{"flat", "sound", "why"}`, next to
`samples.csv`. A run is not sound if it failed, left a path of the mix unexercised or did not
exit cleanly. `--rejudge` with several files refuses any segment whose `result.json` is not
sound or is missing, and exits 2. With one file and no `result.json`, as for runs from before
this change, it judges as before.

## Consequences

- The acceptance soak is "6 h" of load after warm-ups, in two segments of 3 h or more. A
  dispatch with `hours=6` makes two 3 h segments. With `parallel` at 2 or more they run side by
  side, and the run takes about 3.5 h.
- No process is tested for more than about 5 h. Anything that fails only with age, not with
  work (a timer wheel that wraps, a cache that fills only after a day, a certificate reloaded
  daily), is outside what a relay can show. The production canary and its metrics are where such
  a problem would appear.
- Segments that run in parallel are independent repeats on different machines, not one longer
  run. Their noise does not compound, and the shared slope's standard error shrinks with every
  segment added.
- A soak of N segments uses N hosted jobs out of the account's 20 at once, for up to 5.25 h each.
  `parallel` defaults to 4 so that CI keeps most of the slots.
- The io_uring jobs depend on the hosted kernel. If an image update breaks provided buffer rings
  (as Ubuntu's 6.8 kernel did on the old host), `reactor-matrix`'s probe names the cause.

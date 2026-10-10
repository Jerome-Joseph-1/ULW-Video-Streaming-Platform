# CI lanes and batch merging

How `.github/workflows/ci.yml` splits its jobs between pull requests and batches, why, and how
the merge loop lands pull requests. The coverage job's triggers here refine ADR-0086's.

## Why

ci.yml ran its 20 jobs on every pull request and every push to main: three `build-test`
configurations, `tsan`, two `reactor-matrix`, two `integration`, `coverage` (with the SonarQube
Cloud scan), `cluster`, `ingest`, four `lint` shards, two `autobahn`, `fuzz`, `secrets` and
`manifests`. security.yml adds four more (CodeQL and three that take seconds).

GitHub's plan for a personal account runs at most 20 hosted jobs at once across the whole
account. One ci.yml run fills that alone, so with several pull requests open every run queues
behind the others, and a pull request waits for a slot long before it waits for its tests. GitHub's
merge queue, which would test the merged result once per group, is not available for
repositories owned by a personal account.

Measured on recent successful runs (job wall time, minutes):

| Job | Pull requests | Pushes to main |
|---|---:|---:|
| `coverage` (build, unit and integration labels, report, scan) | 13.0-16.2, median 14.1 (9 runs) | 14.5-16.8 |
| `build-test (gcc/ci)` | 9.0-11.3 | 9.2-10.0 |
| a `lint` shard, changed files only | 1.6-4.9 | 26-28 (full tree) |
| security.yml `codeql` | 7.4-14.1 | |

The owner requires SonarQube Cloud's 80% gate on new code to be real coverage: the gate is
decided on the pull request's analysis, which reads the coverage the `coverage` job measures for
that pull request (sonar-project.properties, ADR-0079, ADR-0086).

## Options considered

| Option | Why it was tempting | Verdict |
|---|---|---|
| Keep every job on every pull request | Nothing changes; each pull request is proved alone | Rejected: one run fills the 20-job limit, so open pull requests queue behind each other, and each is still proved only against the main it branched from, not against the others it lands with |
| GitHub merge queue | Tests the merged result once per group, built in | Rejected: not available for personal-account repositories |
| A quick lane on pull requests, the full suite on a batch branch built by a merge loop, and on main | Pull requests use 4-6 slots; the full suite runs once per batch, on the exact tree main will have | Accepted |
| Coverage and the Sonar gate in the batch lane only | Quick lane under ten minutes | Rejected: the gate on new code is enforced per pull request, by its own analysis; a batch branch's analysis is not a pull request's and decorates nothing. At a median of 14 min the job fits the quick lane |
| dorny/paths-filter for the path filters | Familiar | Rejected: a third-party action and its pin for what one `git diff` and a `case` do |
| Move CodeQL off pull requests | One fewer job per pull request | Rejected: it is one job, about ten minutes, and its value is the alert on the pull request |

## The lanes

**Two lanes in ci.yml**, chosen by the first job, `changes`:

- **Quick lane** (`pull_request`). At most seven jobs:
  - `changes`: on a pull request, `git diff --name-only --no-renames origin/<base>...HEAD`.
    `code` is false when every changed path (both sides of a rename) is under `docs/`,
    `deploy/` or `.github/`, or ends in `.md`; `deploy` is whether any path is under `deploy/`;
    `web-mls` is whether any is under `clients/web-mls/`. Everything else, `clients/web-mls/`
    included, is code: the SonarQube scan in `coverage` analyses them.
  - `build-test (gcc/ci, unit)`: the ci preset with gcc, the unit label, then the binary
    hardening check and the privilege-drop tests as root, as the full lane's gcc job runs them.
    Only when `code`.
  - `boundaries + format + tidy (0/1)`: one lint shard over the changed files: boundaries, format, the E2EE diagnostic,
    the OpenMLS bridge's checks, the browser MLS client's checks (only when `web-mls`), the
    shard self-check, clang-tidy on the changed files, and check-docs. Always.
  - `coverage`: as before (ADR-0086), and the SonarQube Cloud scan, on pull requests from this
    repository and those labelled `coverage`. Only when `code`. It is part of the quick lane:
    the gate on new code is decided on the pull request's own analysis, and at 13-16 min it is
    the lane's longest job.
  - `secrets`: always.
  - `manifests`: only when `deploy`.
  - `web-mls-dist`: rebuilds `clients/web-mls/dist/` in its pinned image and compares it byte for
    byte (ADR-0098), about two minutes. Only when `web-mls`.
  A pull request that changes code takes about 15 min (coverage) on five to seven jobs; one that
  changes only docs, Markdown, deploy/ or .github/ runs `changes`, `lint` and `secrets` (and
  `manifests` for deploy/) in about five.
- **Full lane** (`push` to `main` or `batch/**`, the nightly schedule, `workflow_dispatch`).
  `changes` and every job as before: `build-test` ×3, `tsan`, `reactor-matrix` ×2,
  `integration` ×2, `coverage`, `cluster`, `ingest`, `lint` ×4 over the full tree, `autobahn`
  ×2, `fuzz` (300 s per target; 1800 s on the nightly and manual runs), `secrets`,
  `manifests`, `web-mls-dist`. No path filter applies. On a `batch/**` push `coverage` measures
  and reports but does not scan: the batch's tree reaches main as squash commits, and main's
  push scans them. `fuzz` no longer runs on pull requests.

**security.yml** keeps its triggers: pull requests, pushes to main, weekly and manual. Its
CodeQL job stays on pull requests for its alerts there; the other three take seconds. It does not
run on `batch/**`: a code-scanning analysis per batch branch would only add noise.

**e2e.yml** also runs on pushes to main, beside the manual, nightly and `phase-*` runs. Its
concurrency group keeps one run going and at most one waiting.

**The batch merge process.** A merge loop, run by the owner, lands approved pull requests:

1. Take the queued pull requests, oldest first, whose quick lane is green.
2. Build `batch/<N>` from main's tip by merging each queued pull request's head into it, in
   order (`git merge --no-ff`). That is the tree GitHub's squash merges will produce in the same
   order, since each squash is a three-way merge of the head into main as it then is. A pull
   request that does not merge cleanly is dropped from the batch and sent back to its author.
3. Push `batch/<N>`. ci.yml's full lane runs on it.
4. Green: squash-merge each pull request in the same order through the API. Main's final tree
   equals `batch/<N>`'s tree; the loop checks that (`git diff --quiet batch/<N> main`) and stops
   if not, which can only happen when main moved during the run. Delete `batch/<N>`.
5. Red: bisect. Split the batch in halves, build `batch/<N+1>` from the first half, and repeat
   until the failing pull request is found; it goes back to its author and the rest are
   batched again. A batch of one that fails is that pull request's failure.
6. If main moved while the batch ran (a push that did not come through the loop), the batch is
   rebuilt from the new tip and tested again.

**publish-images.yml** publishes a commit on main only once ci.yml's run for that commit's push
to main has succeeded (`event=push&branch=main`); that success starts it (see "Where jobs run").
Every squash merge is a push to main, and every push to main runs the full lane, so a merged
commit still gets the run the publish needs; a `batch/**` run never satisfies it.

## Where jobs run

<!-- .github/workflows/ci.yml (the runs-on of every job, the "trusted-runner" anchor), .github/workflows/publish-images.yml (on.workflow_run) -->

The self-hosted host is one machine with four runners (`vmi3627724`, `-2`, `-3`, `-4`), so four
jobs at a time share its cores. On 2026-10-09/10 the full lane of four pull requests and two
batches queued about 45 jobs on it: a full-tree clang-tidy shard took 2-2.5 h and hit the lint
job's 150-minute limit, main's own run on the #166 merge started none of its self-hosted jobs in
five hours, and publish-images, which then ran beside ci and waited for it, gave up twice
(285 min) with nothing published. Three changes followed:

| Change | Why |
|---|---|
| `lint` (the four clang-tidy shards and the quick checks), `secrets` and `manifests` run on a hosted runner, whatever the event | None needs the host: they compile nothing, use no io_uring and lock no memory. The repository is public, so hosted minutes cost nothing. A shard takes 15-24 min there; lint's limit is 60 min again. Forks' pull requests ran them there already |
| Main's runs (its pushes and the nightly) may take any of the host's runners; every other run only those carrying the labels in the repository variable `ULW_SHARED_RUNNERS` (a JSON list, `["self-hosted","ulw-shared"]`) | A runner without the `ulw-shared` label serves main alone, so main's run, and the publish that waits on it, never queue behind pull requests and batches. With the variable unset every run takes any runner, as before |
| publish-images starts on ci's success (`workflow_run`, for a run of a push to main) instead of beside it | Nothing waits on ci any more, so a ci run of any length gets its commit published; a successful re-run of a failed run publishes it too. The images now come about 45-60 min after ci, the build no longer overlapping it |

The jobs left on the host are the ones that need its kernel: `build-test`, `tsan`,
`reactor-matrix`, `autobahn` and `fuzz`. A pull request from a fork still runs them on a hosted
runner, never on the host.

**Reserving a runner for main** (the repository's admins): on GitHub, Settings, Actions,
Runners, add the label `ulw-shared` to three of the four runners (each runner's `...` menu,
"Edit labels"), leaving one without it; then Settings, Secrets and variables, Actions,
Variables, add `ULW_SHARED_RUNNERS` with the value `["self-hosted","ulw-shared"]`. In that
order: with the variable set and no runner labelled, pull requests' and batches' jobs wait for a
runner that never comes. Deleting the variable undoes it.

## Consequences

- A pull request uses 3-6 hosted jobs instead of 20, plus security.yml's four. The 20-job limit
  is mostly spent on batches and main, once per batch rather than once per pull request.
- The full suite (both compilers, ASan, TSan, both reactors, integration, cluster, ingest,
  Autobahn, fuzzing, clang-tidy over the whole tree) runs before merge on exactly the tree main
  will have, which no per-pull-request run did when several landed together.
- A green quick lane does not mean the full suite passes; the batch is the gate for merging. A
  pull request that wants the full suite earlier can be run with "Run workflow" on its branch.
- A pull request that changes only `.github/` skips the build: a change to ci.yml or the setup
  action is proved by its batch (or a manual run on its branch), not by its quick lane.
- Squash-merging a batch of N pushes N commits to main. ci.yml's concurrency keeps one main run
  going and replaces a waiting one, so the first and last commits get full runs and the ones
  between may be cancelled; publish-images.yml, started by each successful one, keeps one publish
going and replaces a waiting one in the same way (docs/adr/0085). The last
  commit's tree is the tested batch tree, so the commit that is published and that `main` points
  at is one that ran the full suite.
- Each `batch/**` push saves its own ccache entries; actions/cache lets a branch restore main's
  but not another batch's, so batches start from main's cache, as pull requests do.
- SonarQube Cloud has no analysis of `batch/**` branches; its main branch is analysed on each
  push to main, as before.

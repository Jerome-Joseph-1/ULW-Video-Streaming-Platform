# 0086. Coverage is measured once for the report and the SonarQube scan, and the integration label runs one test per core

Status: Accepted
Date: 2026-10-03

## Context

Measured on main (ci.yml run 37093529771, sonar.yml run 37093529808), the slowest checks after
clang-tidy were:

| Job | Wall time | Where it went |
|---|---:|---|
| sonar.yml `analyse` | 28.9 min | `tools/coverage.sh` 24.1 (unit 5.9, integration 18.2), scan 1.9, build 1.1 |
| ci.yml `coverage` | 26.5 min | `tools/coverage.sh` 24.8 (unit 6.0, integration 18.8), build 0.3 |
| `integration (asan)` | 25.7 min | the integration label 21.0, conformance 2.1, build 1.4 |
| `integration (ci)` | 21.5 min | the integration label 17.8, conformance 0.7, build 1.4 |
| `build-test (gcc/ci)` | 19.0 min | build 10.6 (ccache: 70 hits of 464), all tests 7.6 |

Four causes:

- **The same measurement twice.** ADR-0079 gave the scan a coverage run of its own, rejecting
  ci.yml's artifact because ci.yml's coverage job was off unlabelled pull requests and taking
  another workflow's artifact means a `workflow_run` trigger. Both jobs built the same preset and
  ran the same 2,600 tests one at a time.
- **One test at a time.** The integration label is about 1,070 s (ci), 1,260 s (asan) and
  1,125 s (coverage) of tests run serially, because every Postgres suite carried
  `RESOURCE_LOCK postgres`: the pool test pauses the shared server, and the bucket sweep of
  `S3StoreLive` aborted every old upload in the shared bucket.
- **One test waiting on a poll.** `WorkerTest.FiftySequentialJobsLeakNoWorkspaceNorDescriptor`
  took 275 s, about 5.3 s a job: the test queued each job with a bare `INSERT`, without the
  `NOTIFY job_available` the gateway's commit sends, so the idle worker found it only at the end
  of its 5 s poll.
- **A ccache that never held the build.** Seven jobs share the `gcc-ci` key, and three of them
  (`cluster`, `ingest`, `autobahn`) build a few targets. actions/cache never replaces a key, so
  the first job to finish (`autobahn`, 1.5 min) saved a 5 MB cache under the run's key and
  `build-test` failed to save its own ("another job may be creating this cache"). Every run then
  restored that, and built nearly from scratch.

GitHub's free plan runs at most 20 hosted jobs at once, and one ci.yml run is already about that
many: splitting work across more jobs makes runs queue.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| sonar.yml on `workflow_run` of ci.yml, scanning ci.yml's artifacts | Two workflows stay apart | Rejected: a `workflow_run` run has the base repository's secrets for every pull request, forks' included, so the token would sit beside a fork's artifacts (untrusted input) |
| The scan in ci.yml's coverage job, which builds and measures | One measurement, no artifact handoff; the token stays in one step's env on `pull_request` and `push`, as in sonar.yml | Accepted |
| Shard the labels across runners (`ctest -I i,,n`) | Each shard keeps one test at a time | Rejected: seven more jobs per run against the 20-job limit |
| `ctest -j` with the conflicts named on the tests that have them | Same job count; uses the runner's four cores | Accepted |
| `ctest -j` with every Postgres suite still locked | No audit needed | Rejected: almost the whole label stays serial |

## Decision

- **One coverage run.** `sonar.yml` is removed. ci.yml's `coverage` job builds the `coverage`
  preset, runs `tools/coverage.sh build/coverage unit integration` with
  `COVERAGE_PARALLEL=integration` (the integration label one test per core, the unit label one
  at a time as `build-test` runs it), writes the report as before, and then runs ADR-0079's
  Python and Go coverage, pinned scanner and scan. It runs on pushes to main, the nightly
  schedule, manual dispatch, pull requests from this repository (where sonar.yml measured) and
  pull requests labelled `coverage` (ADR-0080). A fork's unlabelled pull request runs it not,
  as before. The scan is skipped on the nightly run (sonar.yml had none).
- **The token.** As ADR-0079: the first step sees only whether `SONAR_TOKEN` is set, its value is
  in the scan step's env and nowhere else, and forks' and Dependabot's pull requests get no
  secrets, so their runs skip the scan. The workflow keeps `permissions: contents: read`. No
  `workflow_run` or `pull_request_target` is added.
- **Profiles under -j.** Every instrumented binary already writes to
  `<build>/coverage/profiles/ulw-%8m.profraw` (ADR-0080): a pool of files named by the binary's
  signature, merged online under a lock, so concurrent processes never clobber each other.
- **The integration label under -j.** `ctest -L integration -j "$(nproc)"` in the integration
  jobs and the coverage job. What the tests share, and how each is handled:
  - Postgres: every test has a scratch database (`ScratchDatabase`), dropped after it; the
    blanket `RESOURCE_LOCK postgres` is gone. Queries on `pg_stat_activity` are limited to
    `current_database()`; the one that was not (the upload reaper's lock wait) now is.
    Advisory locks are per database.
  - The pause: `PoolTest.PausedDatabaseNeverStallsTheLoop` docker-pauses the server, which would
    stall every other test, and measures the event loop's slices; it is `RUN_SERIAL`
    (`ulw_add_test_suite(... SERIAL <filter>)` in `tests/CMakeLists.txt`).
  - Connections: one test per core opens up to about 130 sessions at once (measured), past the
    image's default `max_connections` of 100. `deploy/local/compose.yaml` starts Postgres with
    `max_connections=400`; ci.yml sets the same on its service container (`ALTER SYSTEM` and a
    restart, since a service takes no command line).
  - MinIO: every test writes under a `unique_prefix`, and the two that sweep a whole bucket
    (`UploadReaperLive`, now also `S3StoreLive`) make a bucket of their own and delete it.
  - Ports: every server a test starts takes ports it reserved, retrying on a lost race
    (`tests/support/child_process.hpp`); the cluster's pinned ports apply only when
    `ULW_CHAT_CLUSTER_PORTS` is set, which only the `cluster` job does, running alone.
  - Files: scratch and output directories are `mkdtemp` directories per test.
  The conformance label still runs one test at a time, after the integration label.
- **The worker test** queues its jobs the way the gateway's commit does, with `NOTIFY
  job_available`: the same 51 jobs and every assertion, in about 30 s instead of 275.
- **ccache.** The setup action takes `ccache-save: "false"`, which restores with
  `actions/cache/restore` and saves nothing. `cluster`, `ingest` and `autobahn` use it, so the
  `gcc-ci` key is saved by a full build.

## Consequences

- Measured locally (4 cores, ci preset, Postgres with `max_connections=400` and MinIO from
  compose): the integration label in about 4.5 min with `-j4` against 18 min one at a time on
  the runner, five runs in a row without a failure. Expected on the runners: `integration (ci)`
  about 9 min, `integration (asan)` about 11, `coverage` about 17, `build-test (gcc/ci)` about 10
  once its cache holds the build.
- The job count is unchanged by this decision, and one lower with sonar.yml gone.
- A new integration test that touches something process-wide or server-wide (pausing or
  restarting a server, a fixed port, a whole bucket, a cluster-wide setting) needs `SERIAL` or
  its own resource; one that only uses its scratch database, a unique prefix and reserved
  ports does not.
- A failure seen only under `-j` is a finding about a test's isolation, to be fixed in the test,
  never retried.
- The check is now `ci / coverage`; `sonar / analyse` no longer exists.

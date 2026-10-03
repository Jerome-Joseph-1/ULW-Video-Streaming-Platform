# 0083. Coverage and the SonarQube scan share one sharded run, and the integration label is split across runners

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

Three causes:

- **The same measurement twice.** ADR-0079 gave the scan its own coverage run, rejecting ci.yml's
  artifact because ci.yml's coverage job was off unlabelled pull requests and another
  workflow's artifact means a `workflow_run` trigger. Both jobs build the same preset and run
  the same 2,600 tests one at a time.
- **Serial labels.** The integration label is about 1,070 s (ci), 1,260 s (asan) and 1,125 s
  (coverage) of tests run one at a time; one test, `WorkerTest.FiftySequentialJobsLeak...`, is
  275 s of it. `-j` stays off: the pool test pauses the shared Postgres and the reaper test
  aborts uploads in the shared bucket, and several suites time servers.
- **A ccache that never held the build.** Seven jobs share the `gcc-ci` key, and three of them
  (`cluster`, `ingest`, `autobahn`) build a few targets. actions/cache never replaces a key, so
  the first job to finish (`autobahn`, 1.5 min) saved a 5 MB cache of one target under the
  run's key, and `build-test` and `reactor-matrix` failed to save theirs ("another job may be
  creating this cache"). Every run then restored that, and built nearly from scratch. The seven
  `fuzz` jobs share `clang-fuzz` the same way.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| sonar.yml on `workflow_run` of ci.yml, scanning ci.yml's artifacts | Two workflows stay apart | Rejected: a `workflow_run` run has the base repository's secrets for every pull request, forks' included, so the token would sit beside a fork's artifacts (untrusted input: a compile database and reports the fork wrote); and it needs an `actions: read` token |
| The scan as a job in ci.yml, after the coverage job, on its artifacts alone | No build in the job with the token | Rejected: the CFamily analyser reads the compile database's sources, generated headers and FetchContent trees at their build paths and runs the compiler to probe it, so the artifact would be most of the build tree plus the toolchain; same-repository pull requests, the only ones the token reaches, are written by people who can push anyway |
| The scan in the job that merges the coverage shards (rebuilt from their ccache, no test run) | One measurement; the token stays where ADR-0079 put it, in one step's env on `pull_request` and `push` | Accepted |
| Run the integration label with `ctest -j` | One job | Rejected: the tests share one Postgres and one bucket and are written to run alone |
| Split each serial label across runners (`ctest -I i,,n`), each with its own Postgres and MinIO | Still one test at a time per server; every test runs once | Accepted |
| Split by a committed per-test timing file | Better balance | Rejected for now: a file to keep current; striding already spreads each suite's tests across shards |
| Save ccache only on main | Less cache churn | Rejected: the coverage report job restores the shards' cache saved minutes earlier in the same pull request run |

## Decision

- **One coverage run.** `sonar.yml` is removed. ci.yml's `coverage-shard` jobs (four) each build
  the `coverage` preset and run `COVERAGE_SHARD=i/4 tools/coverage.sh build/coverage unit
  integration`: every fourth test of each label from the i-th, one at a time, against their own
  Postgres and MinIO, merged into `ulw.profdata`, uploaded with the `clang-19` package version
  that built it. The `coverage` job then rebuilds the same configuration from the shards' ccache
  (the same sources, preset and compiler give the same binaries, which the profiles must
  match), refuses a profile from another clang, and runs `COVERAGE_PROFILES=<dir>
  tools/coverage.sh build/coverage`, which merges the shards' profiles and writes the same
  report as before (step summary, `coverage-html` artifact, `llvm-cov-show.txt` for the scan).
  The python and Go coverage, the pinned scanner and the scan are ADR-0079's steps, moved.
- **Where it runs.** Both run on pushes to main, the nightly schedule, manual dispatch, pull
  requests from this repository (where sonar.yml used to measure) and pull requests labelled
  `coverage` (ADR-0080). A fork's unlabelled pull request runs neither, as before. The scan is
  skipped on the nightly run (sonar.yml had none).
- **The token.** As ADR-0079: the first step sees only whether `SONAR_TOKEN` is set, its value is
  in the scan step's env and nowhere else, and forks' and Dependabot's pull requests get no
  secrets, so their runs skip the scan. The workflow keeps `permissions: contents: read`; the
  artifacts come from the same run, which needs no token. No `workflow_run` or
  `pull_request_target` is added.
- **A failed shard** fails its own job; the `coverage` job still reports and scans what it has,
  warns when fewer than four profiles arrived, and then fails, as the single job failed on a
  failed test.
- **Integration shards.** `integration (preset, i/3)` runs `ctest -L integration -I i,,3`, one
  test at a time with its own Postgres and MinIO; storage conformance runs on shard 1 after
  its share of the label. Every test makes its own scratch database, so none depends on another
  having run before it.
- **ccache.** The setup action takes `ccache-save: "false"`, which restores with
  `actions/cache/restore` and saves nothing. `cluster`, `ingest` and `autobahn` use it, so
  `gcc-ci` is saved by a full build. Each `fuzz` target has its own key.

## Consequences

- Expected wall time per job, from the measured step times: `coverage-shard` about 12 min
  (setup and cached build 2, a quarter of the unit label 1.5, the shard with the 275 s test 8);
  `coverage` about 5 min after them; `integration (ci, i/3)` about 13 and `integration (asan,
  i/3)` about 15; `build-test (gcc/ci)` about 10 once its cache holds the build. The longest
  path on a pull request is then the coverage pair, about 17 min.
- More runners per run: four coverage shards and the report job where there were two coverage
  runs, six integration jobs where there were two. The minutes are about the same; the setup
  and cached build are paid per shard.
- A test added to the integration label lands in a shard by its position; a shard can drift
  longer as tests are added. Raise the shard count, in the matrix and the `-I` stride together,
  when one passes 15 minutes.
- A change to the clang-19 package between a shard and the report job fails the report job
  instead of producing numbers from mismatched binaries; re-running the workflow fixes it.
- The check is now `ci / coverage` (and the shards); `sonar / analyse` no longer exists.

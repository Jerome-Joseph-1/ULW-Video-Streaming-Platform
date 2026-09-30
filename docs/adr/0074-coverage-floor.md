# 0074. Coverage is measured on every pull request and held to a floor per top-level directory

Status: Accepted
Date: 2026-09-30

## Context

Nothing measured which lines and branches the tests run. The unit and integration labels are
large (about 2,000 and 320 tests), and several suites start the servers themselves
(`gateway`, `chat_server`, `live_packager`, `transcode_worker`) as child processes, so what a
server's `main` and its error handling do under test was unknown. Coverage could fall with any
change and nothing would say so.

The toolchain is already clang-19 beside gcc-14 (`.github/actions/setup`). The harnesses start
each server with an environment they build themselves, not the test's (`ChildProcess::start`),
so anything passed to a child through the environment does not arrive. The first measurement
passed the profile path through `LLVM_PROFILE_FILE` and read `apps/gateway/src/main.cpp`,
`apps/chat/src/main.cpp` and `apps/live-packager/src/main.cpp` as 0%, and `apps` as 75.1% of
lines where it is 86.6%.

The host this was measured on was also short of disk: a debug build with debug information is
about three times the size of one without, and the counters need none.

## Options

The tool:

| Option | Why it was tempting | Verdict |
|---|---|---|
| gcc `--coverage` with gcovr or lcov | The gcc-14 builds already exist | Rejected: gcovr is a Python package to pin and install on top, and gcov counts the compiler's own exception edges as branches, untaken in every function that can throw |
| clang source-based coverage (`-fprofile-instr-generate -fcoverage-mapping`), `llvm-profdata` and `llvm-cov` | clang-19 is installed; region and branch counts map to source; `%Nm` merges each binary's profiles online across concurrent processes | Accepted |

Where each process writes its profile:

| Option | Why it was tempting | Verdict |
|---|---|---|
| `LLVM_PROFILE_FILE` in the test's environment | The documented default | Rejected: the servers the tests start never see it |
| The path built into each binary (`-fprofile-instr-generate=<path>`) | Every process of the build writes to one place whatever its environment; `LLVM_PROFILE_FILE` still overrides it | Accepted |

What is held, and how:

| Option | Why it was tempting | Verdict |
|---|---|---|
| A target to reach (say 90%) | Pushes coverage up | Rejected: a target nobody is working towards fails every pull request until someone lowers it |
| One floor for the whole tree | One number | Rejected: a directory can lose a great deal while another gains a little |
| A floor per file | The finest grain | Rejected: splitting or moving a file breaks it, and small files swing by whole percents on one line |
| A floor per top-level directory, at the measured value rounded down to a whole percent | Catches a directory losing tests; the rounding absorbs the few lines that timing decides | Accepted |

## Decision

- The `coverage` preset is clang-19, `Debug` at `-O0` without debug information, with
  `ULW_COVERAGE=ON` (`cmake/Coverage.cmake`): the counters go on every first-party target
  through `ulw_sanitize`, which each links, so llhttp, googletest and srt are not instrumented.
  Each binary writes to `<build>/coverage/profiles/ulw-%8m.profraw`, at most eight files per
  binary however many processes a suite starts.
- `tools/coverage.sh <build> [label...]` runs the labels (unit with all cores, integration one
  test at a time, as the integration job does), merges the profiles, and reports on every
  instrumented binary in the tree, test suites and servers alike; a source file that only a
  binary no test ran is counted at zero. `tests/`, `third_party/` and the build tree's generated
  sources (build info, the bundled migrations) are left out. It writes the HTML report,
  `summary.json` and `summary.md`, and fails when a test fails or a directory is below its floor.
- The `coverage` job in `ci.yml` runs it on pull requests and on pushes to main, against the
  same Postgres and MinIO as the integration job, with `llvm-19` pinned to the version of the
  installed `clang-19`. The table goes to the step summary and the HTML report is an artifact.
  It has a ccache key of its own (instrumented objects never match another job's), which pull
  requests restore from main's last run.
- The floors are in `tools/coverage-floors.txt`, measured on `f2fb1d4` (unit and integration
  labels, io_uring reactor, not root):

| Directory | Lines | Line floor | Branches | Branch floor |
|---|---:|---:|---:|---:|
| apps | 86.6% | 86 | 79.7% | 79 |
| codec | 95.7% | 95 | 85.1% | 85 |
| core | 91.4% | 91 | 83.9% | 83 |
| http | 94.3% | 94 | 94.9% | 94 |
| infra | 85.8% | 85 | 74.7% | 74 |
| net | 86.9% | 86 | 73.9% | 73 |
| ops | 91.1% | 91 | 84.3% | 84 |
| os | 46.2% | 46 | 24.4% | 24 |
| rt | 88.8% | 88 | 77.9% | 77 |
| tools | 52.7% | 52 | 43.4% | 43 |

## Consequences

- A floor is raised by hand when a change lifts its directory past the next whole percent, and
  never lowered; a pull request that drops a directory below its floor fails until it adds the
  tests or the change is reconsidered.
- A new top-level directory is reported without a floor until one is added for it.
- `os` reads low because its privilege tests need root, which only `build-test`'s separate root
  step has; that step is not measured.
- The conformance label, the cluster, ingest and Autobahn jobs, the fuzzers and the soaks are not
  measured; code only they exercise reads as uncovered here.
- Not measured either: code that only runs where a test cannot reach it without failing an
  allocation (`bad_alloc` handlers), which accounts for much of what `apps/chat` leaves
  uncovered.

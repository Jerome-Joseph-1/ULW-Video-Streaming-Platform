# 0078. SonarQube Cloud analysis in CI, with the compile database, on the Free plan

Status: Accepted
Date: 2026-10-02

## Context

SonarQube Cloud (sonarcloud.io) analyses this public repository on its Free plan and keeps a
dashboard of duplication, complexity and issues over time, and decorates pull requests with the
new issues they add. Until now it ran as Automatic Analysis: SonarQube Cloud cloned the
repository and analysed it on its own, configured by `.sonarcloud.properties` (which only split
product code from tests).

Automatic Analysis cannot build the C++. It does not know our compile flags, cannot see system
headers such as `curl/curl.h`, nor the headers the build generates, so its C++ results are a
guess. Some findings cannot be cleared at all: cpp:S4423 (weak TLS protocol) on a curl option set
to TLS 1.3 stays open, because without curl's header the analyser cannot tell what the constant
is. Analysis run in CI, with the compile database of a real build, sees exactly what the
compiler sees.

CI already checks the C++ with clang-tidy, and ADR-0072 adds CodeQL. Constraints from the rest
of CI: every action is pinned by commit SHA and every download by version and SHA-256; jobs get
least privilege; no secret is committed, and none is needed for CI to be green. The owner keeps
the project on the Free plan: no setting may depend on a paid plan.

## Options

Where the analysis runs:

| Option | Why it was tempting | Verdict |
|---|---|---|
| Keep Automatic Analysis | Nothing to add to CI | Rejected: no compile flags, system or generated headers, so C++ findings are guesses and some cannot be cleared |
| A step in `ci.yml` | One workflow | Rejected: `ci.yml` is already heavy, and this job must be free to skip or fail without touching it |
| A new workflow, `sonar.yml`, with its own build of the `ci` preset | Independent; reuses the setup action and the `gcc-ci` ccache | Accepted |

How C++ is analysed:

| Option | Why it was tempting | Verdict |
|---|---|---|
| The build wrapper | SonarSource's older default | Rejected: another binary to download and pin, for what CMake already writes |
| The compile database, `build/ci/compile_commands.json` (`sonar.cfamily.compile-commands`) | The `ci` preset writes it (`CMAKE_EXPORT_COMPILE_COMMANDS`); the same file clang-tidy reads | Accepted |

How the scanner is fetched:

| Option | Why it was tempting | Verdict |
|---|---|---|
| Let the scan action download it | Its default | Rejected: it checks a GPG signature against a key fetched from a keyserver, not a SHA-256 pinned here |
| Download the zip ourselves by version and SHA-256, and put it where the action's tool cache looks | Same pin as every other download; the action finds it and downloads nothing | Accepted |

Whether the quality gate fails the job:

| Option | Why it was tempting | Verdict |
|---|---|---|
| `sonar.qualitygate.wait=true` | A gate that blocks | Rejected for now: the gate's default conditions have never been tuned to this code |
| `sonar.qualitygate.wait=false`: advisory | The result shows on the dashboard and the pull request; the job's status is the scan's | Accepted |

## Decision

`.github/workflows/sonar.yml` runs on pushes to main, pull requests and manual dispatch, with
`permissions: contents: read` only: SonarQube Cloud decorates pull requests through its own
GitHub App, so the job needs no `pull-requests` scope.

- **Skip.** The first step sees only whether `SONAR_TOKEN` is set (`secrets.SONAR_TOKEN != ''`
  in its env, not the value), and every later step is conditional on it. On pull requests from
  forks and Dependabot, which get no secrets, the job passes with a notice and does nothing else.
- **Checkout.** `fetch-depth: 0` for blame and new-code detection, `persist-credentials: false`.
- **Build.** The repository's setup action, `CC=gcc-14`, cache key `gcc-ci` (shared with
  reactor-matrix), `cmake --preset ci` and a full `cmake --build --preset ci`, so every
  generated header and source the compile database names exists.
- **Scanner.** SonarScanner CLI 8.1.0.6389, linux-x64, from Maven Central, checked against
  SHA-256 `bb8f709f…795499d0b`. That hash is in the action's own `sonar-scanner-version` file at
  the pinned commit and in Maven Central's `.sha256`, and was recomputed from a download. The zip
  is unpacked to `$RUNNER_TOOL_CACHE/sonar-scanner-cli/8.1.0-build.6389/linux-x64` with its
  `.complete` marker, the path the action's tool-cache lookup checks. The action gets the same
  `scannerVersion`, and a `scannerBinariesUrl` under `.invalid`: were its cache layout ever to
  stop matching, it would fail to download rather than fetch something unchecked.
- **Scan.** `SonarSource/sonarqube-scan-action@ba9859eae8dd6bd29e412f25ddbbef3d032000f4`, tag
  v8.2.2 (checked with `git ls-remote`). `SONAR_TOKEN` is in this step's env and nowhere else.
  Branch and pull request parameters are not set: the scanner reads them from the GitHub Actions
  environment and event.
- **`sonar-project.properties`.** Organisation `jerome-joseph-1`, project key
  `Jerome-Joseph-1_ULW-Video-Streaming-Platform`. The source and test split is the one
  `.sonarcloud.properties` had: `tests/**` and `**/*_test.py` as tests and the rest as sources.
  `build/` and the vendored tarballs in `third_party/` are left out; files git ignores are left
  out by the scanner. The compile database, whose `-std=c++23` sets the analysed standard; C and
  C++ files the `ci` preset does not compile (the fuzz targets, `tools/io_uring_probe.c`) are not
  analysed. `sonar.qualitygate.wait=false`. No thread count: the
  analyser uses every core by default. A marked block names the coverage properties for when a
  coverage job exists.
- **`.sonarcloud.properties` is removed.** Only Automatic Analysis reads it; with CI analysis it
  would be a second, ignored copy of the settings.

Only Free plan features are used: C and C++ analysis, main branch and pull request analysis of a
public repository, the default quality gate, and the properties above.

Overlap: clang-tidy stays the gate for C++ correctness, and CodeQL (ADR-0072) for data flow and
security. Where a SonarQube Cloud rule repeats a clang-tidy check, mark the SonarQube Cloud issue
with a pointer to the check that owns it, rather than tuning either tool to hide the other.

What the owner does, once, before this merges: in the project's Administration, Analysis
Method, turn **Automatic Analysis off** (with it on, the CI scan is refused); `SONAR_TOKEN` is
already a repository secret. Then check the organisation and project keys on the project's
Information page against `sonar-project.properties`.

## Consequences

- C++ findings are made with the real flags and headers; findings that only stood because the
  analyser could not see a system header can now be cleared by the next analysis.
- The job rebuilds the `ci` preset, about as long as a reactor-matrix build with a warm ccache,
  then analyses some 400 translation units, which adds minutes; it is limited to 90.
- Pull requests from forks get no analysis; their results arrive with main's after they merge.
- The scanner is pinned; the scanner engine and analysers it downloads from SonarQube Cloud at
  run time are chosen by the service and cannot be pinned here.
- While Automatic Analysis is on, this job fails at the scan step. That is the signal to turn it
  off, not to remove the job.
- The quality gate is advisory. Making it blocking is a later decision: set
  `sonar.qualitygate.wait=true` and make the check required, once the gate's conditions fit.
- If the repository ever becomes private, the Free plan's limits apply. To stay free, delete the
  `SONAR_TOKEN` secret: the job then skips itself.
- Bumping the action or the scanner means a new commit SHA, a new version, and its SHA-256 from
  Maven Central's checksum; the action's `sonar-scanner-version` file lists the version it was
  tested with.

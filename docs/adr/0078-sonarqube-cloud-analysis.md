# 0078. SonarQube Cloud analysis on the Free plan, advisory, skipped until the owner opts in

Status: Accepted
Date: 2026-09-30

## Context

CI checks the C++ with clang-tidy (the clang-analyzer, bugprone, cert and concurrency checks,
changed files on pull requests and the full tree on main), and ADR-0072 adds CodeQL for
interprocedural data flow. Both answer "is this change wrong?" and report in the run's log or
in code scanning alerts. Neither keeps a view over time: how duplication, complexity, and the
count and kind of issues move from one commit to the next, which parts of the tree carry most
of them, and whether new code is cleaner than the code around it.

SonarQube Cloud (sonarcloud.io) keeps that dashboard and its history, and decorates pull
requests with the new issues they add. Its Free plan analyses public repositories, C and C++
included, with branch and pull request analysis, at no cost. The owner wants it on that plan
and no other: no setting may depend on a paid plan.

Constraints from the rest of CI: every action is pinned by commit SHA and every download by
version and SHA-256; jobs get least privilege; no secret is committed, and none is needed for
CI to be green. The owner creates the SonarQube Cloud organisation and the token, not the
repository.

## Options

Where the analysis runs:

| Option | Why it was tempting | Verdict |
|---|---|---|
| SonarQube Cloud's Automatic Analysis | Nothing to add to CI | Rejected: it cannot see our compile flags or generated headers, so its C++ results are a guess; it must be turned off for CI analysis to run at all |
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
| Let the scan action download it | Its default | Rejected: it checks a GPG signature against SonarSource's key fetched from a keyserver by a fingerprint in the action, not a SHA-256 pinned here |
| Download the zip ourselves by version and SHA-256, and put it where the action's tool cache looks | Same pin as every other download; the action finds it and downloads nothing | Accepted |

Whether the quality gate fails the job:

| Option | Why it was tempting | Verdict |
|---|---|---|
| `sonar.qualitygate.wait=true` | A gate that blocks | Rejected for now: the gate's default conditions have never been tuned to this code, and a red job on day one teaches people to ignore it |
| `sonar.qualitygate.wait=false`: advisory | The result shows on the dashboard and the pull request; the job's status is the scan's | Accepted |

## Decision

`.github/workflows/sonar.yml` runs on pushes to main, pull requests and manual dispatch. It has
`permissions: contents: read` only: SonarQube Cloud decorates pull requests through its own
GitHub App, so the job needs no `pull-requests` scope.

- **Skip.** The first step sees only whether `SONAR_TOKEN` is set
  (`secrets.SONAR_TOKEN != ''` in its env, not the value, and not a `secrets` reference in an
  `if:`), and writes `present=true` or `false`. Every later step is conditional on it. Before the
  owner adds the secret, and on pull requests from forks and Dependabot, which get no secrets,
  the job passes with a notice and does nothing else.
- **Checkout.** `fetch-depth: 0` for blame and new-code detection, `persist-credentials: false`.
- **Build.** The repository's setup action, `CC=gcc-14`, cache key `gcc-ci` (shared with
  reactor-matrix, cluster and autobahn), `cmake --preset ci` and a full `cmake --build --preset
  ci`, so every generated header and source the compile database names exists.
- **Scanner.** SonarScanner CLI 8.1.0.6389, linux-x64, fetched from Maven Central and checked
  against SHA-256 `bb8f709f…795499d0b`. That hash is in the action's own `sonar-scanner-version`
  file at the pinned commit, in Maven Central's `.sha256`, and was recomputed from a download.
  The zip is unpacked to `$RUNNER_TOOL_CACHE/sonar-scanner-cli/8.1.0-build.6389/linux-x64` with
  its `.complete` marker, the path `@actions/tool-cache`'s `find` checks. The action gets the
  same `scannerVersion`, and a `scannerBinariesUrl` under `.invalid`: were its cache layout ever
  to stop matching, it would fail to download rather than fetch something unchecked.
- **Scan.** `SonarSource/sonarqube-scan-action@ba9859eae8dd6bd29e412f25ddbbef3d032000f4`, tag
  v8.2.2 (checked with `git ls-remote`). `SONAR_TOKEN` is in this step's env and nowhere else.
- `sonar-project.properties`: organisation `jerome-joseph-1` and project key
  `Jerome-Joseph-1_ULW-Video-Streaming-Platform`, the values SonarQube Cloud generates on import,
  to be confirmed after it; sources `apps codec core http infra net ops os rt tools deploy`,
  tests `tests`; `build/`, `third_party/`, `node_modules`, Cargo `target` directories, fetched
  tools and test data excluded; the compile database; `reportingCppStandardOverride=c++23` (the
  analysed standard itself comes from `-std=c++23` in each entry); `sonar.qualitygate.wait=false`.
  A marked block names the `sonar.cfamily.gcov.reportsPath`, `sonar.cfamily.llvm-cov.reportPath`
  and `sonar.coverageReportPaths` properties for when a coverage job exists; this job builds no
  coverage.

Only Free plan features are used: C and C++ analysis, main branch and pull request analysis of a
public repository, the default quality gate, and the properties above. No portfolio, no
commercial-edition parameter. If a property turns out not to be available on the Free plan,
remove it rather than change plan.

Overlap: clang-tidy stays the gate for C++ correctness, and CodeQL (ADR-0072) for data flow and
security. Some SonarQube Cloud rules repeat clang-tidy's, so the same line can be flagged twice;
mark the SonarQube Cloud issue "Won't fix" with a pointer to the check that owns it, rather than
tuning either tool to hide the other. What only SonarQube Cloud adds is the maintainability
view (duplication, cognitive complexity, issue density by directory) and its history.

What the owner does, once:

1. Sign in to sonarcloud.io with GitHub and import `Jerome-Joseph-1/ULW-Video-Streaming-Platform`
   into the organisation created for the account. Choose the **Free plan**; enter no card
   details.
2. In the project's Administration, Analysis Method, turn **Automatic Analysis off**. With it on,
   CI analysis is refused.
3. Create a token (My Account, Security) and add it as the repository secret `SONAR_TOKEN`
   (Settings, Secrets and variables, Actions).
4. On the project's Information page, check the organisation key and project key against
   `sonar-project.properties`; correct the file if they differ.
5. Re-run the `sonar` workflow on main, or push, to get the first analysis.

## Consequences

- CI stays green before and after the owner acts; until then the job takes seconds.
- Pull requests from forks never get the analysis: they have no secrets. Their results arrive
  with the analysis of main after they merge.
- The job rebuilds the `ci` preset, about as long as a reactor-matrix build with a warm ccache.
- The scanner is pinned; what it downloads from SonarQube Cloud at run time (the scanner engine
  and the language analysers) is chosen by the service, not by this repository, and cannot be
  pinned here. The token grants that service analysis rights on this project only.
- The quality gate is advisory. Making it blocking is a later decision: set
  `sonar.qualitygate.wait=true` and make the check required, once the gate's conditions fit.
- If the repository ever becomes private, the Free plan's private-project limits apply. To stay
  free, delete the `SONAR_TOKEN` secret: the job then skips itself.
- Bumping the action or the scanner means a new commit SHA, a new version, and its SHA-256 from
  the release's checksum file; the action's `sonar-scanner-version` file lists the version it
  was tested with.

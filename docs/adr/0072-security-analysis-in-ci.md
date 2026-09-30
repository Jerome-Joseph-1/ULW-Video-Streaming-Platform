# 0072. Security analysis in CI: CodeQL, osv-scanner, Trivy, workflow lint, hardening check

Status: Accepted
Date: 2026-09-30

## Context

CI already had clang-tidy (the clang-analyzer, bugprone, cert and concurrency checks),
gitleaks over every commit, libFuzzer targets, ASan, UBSan and TSan builds, and kubeconform
over the manifests. Nothing looked at:

- interprocedural data flow in the C++ (tainted sizes into allocations, paths, formats);
- the pinned dependencies against published advisories: two Cargo lock files (the OpenMLS
  bridge, `infra/e2ee/mls_ffi_bridge`, and the test publisher's whipsink), two npm lock files
  (`tests/call`, `tests/e2e`), one hash-pinned pip file (`tests/load/call_capacity`), and three
  vendored tarballs (`cmake/Dependencies.cmake`);
- the manifests' and Dockerfiles' security settings, and the images' OS packages;
- the workflows themselves (permissions, template injection, credentials left in the checkout);
- whether the binaries really are hardened. `cmake/Warnings.cmake` asks for it, but nothing
  checked the result, and the first check found that it was not true for the Clang build: the
  vendored llhttp and srt are separate targets that never got the flags, and Ubuntu's Clang,
  unlike its GCC, enables none of them by default. One object without `-fcf-protection` drops
  the binary's IBT and SHSTK markings, so `gateway_server`, `chat_server` and `live_packager`
  from the Clang ci build had neither, and llhttp and srt had no stack protector,
  `_FORTIFY_SOURCE` or stack-clash protection in either build except by GCC's defaults.

Everything must be free, run on GitHub's runners with no account or token beyond the workflow's
own, and be pinned the way the rest of CI is: actions by commit SHA, downloads by version and
SHA-256.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| CodeQL, `security-extended`, manual build of the ci preset, SARIF to code scanning | Free for a public repository; data flow across translation units, which clang-tidy does not do | Accepted, on its own job |
| Semgrep or Coverity | Other data-flow engines | Rejected: Semgrep's C++ rules are pattern-level; Coverity needs an account |
| osv-scanner over every lock file, with the C and C++ tarballs written as an osv-scanner lock file of upstream commits | One static binary; OSV covers crates.io, npm, PyPI and, by commit range, C and C++ repositories | Accepted |
| cargo-audit, npm audit and pip-audit separately | Each ecosystem's native tool | Rejected: three tools, three allowlist formats, and none for the C and C++ |
| A CycloneDX SBOM for the C and C++ | A standard format | Rejected: OSV matches C and C++ by commit, which purls cannot carry; the lock-file form can |
| Trivy for `deploy/` and the images | One binary for Kubernetes, Dockerfile and image checks; its checks are compiled into the release, so they are pinned with it | Accepted |
| kubescape, checkov, grype | Alternatives for one part each | Rejected: more tools for the same ground |
| actionlint and zizmor over `.github/` | actionlint checks syntax, expressions and (with shellcheck) the run steps; zizmor checks security | Accepted, both |
| cppcheck (`warning,performance,portability`) | A second C++ analyser | Rejected after one run: no signal beyond clang-tidy (below) |
| `tools/check-hardening.sh` with readelf, on the ci build and the release images' binaries | binutils is already on every runner and in the build image; nothing to download | Accepted |
| checksec or hardening-check | Ready-made | Rejected: a download or a Debian-only package for what readelf shows directly |

## Decision

### What runs, and when

| Check | Where | When | Fails on |
|---|---|---|---|
| CodeQL (`c-cpp`, `security-extended`) | `security.yml` `codeql` | pull requests, pushes to main, weekly | the alerts it uploads to code scanning; branch protection decides whether an open alert blocks |
| osv-scanner | `security.yml` `dependencies` (`tools/security/osv-scan.sh`) | pull requests, pushes to main, weekly | any advisory not allowlisted, of any severity |
| Trivy config | `security.yml` `iac` (`tools/security/trivy-config.sh`) | pull requests, pushes to main, weekly | any misconfiguration not allowlisted, of any severity |
| actionlint (with shellcheck), zizmor | `security.yml` `workflows` (`tools/security/lint-workflows.sh`) | pull requests, pushes to main, weekly | any finding |
| Binary hardening, ci build | `ci.yml` `build-test`, both compilers' ci preset | wherever ci.yml runs | any executable under `apps/` or `tools/` lacking PIE, full RELRO, NX, no RWX segment, stack protector, IBT or SHSTK; any first-party, llhttp or srt unit compiled without `-D_FORTIFY_SOURCE=3`, `-fstack-protector-strong`, `-fstack-clash-protection` or `-fcf-protection`; no `__*_chk` import in any executable |
| Binary hardening, release images | `e2e.yml` `sandbox` | nightly, manual, phase tags | the same, on the binaries copied out of the gateway and worker images |
| Trivy image | `e2e.yml` `sandbox` (`tools/security/trivy-image.sh`) | nightly, manual, phase tags | a HIGH or CRITICAL vulnerability with a fixed version, not allowlisted |

The four `security.yml` jobs run in parallel and alongside `ci.yml`. The workflow grants no
permissions at the top; each job asks for `contents: read`, and CodeQL's alone adds
`security-events: write` for the upload. CodeQL builds the ci preset with GCC 14 and
`ULW_BUILD_TESTS=OFF`: the tests would double the build, and nothing they contain ships.
ccache is off for that build, because CodeQL sees only the compiler invocations that run.

The image scan is on the nightly sandbox job because that is the one job that builds the
images; building them on every pull request would add a release build with LTO to each. It
fails on HIGH and CRITICAL with a fix: an unfixed one has nothing to upgrade to, and the
Ubuntu snapshot the Dockerfile pins (`deploy/docker/apt-install.sh`) is what moves when a fix
lands. The unfixed ones are printed in the log all the same. mock-auth's image is the
sandbox's test double and is not scanned.

The hardening check refuses a sanitizer build (by `ULW_SANITIZE` in the cache, or by
`__asan_init`, `__tsan_init` or `__ubsan_handle_*` in the symbols): it ships nowhere, and ASan
changes the layout the check reads. `_FORTIFY_SOURCE` leaves a mark only where a call's bounds
were unknown at compile time, so a binary without a `__*_chk` import proves nothing; hence the
compile-database check per unit and the one-import floor across all executables.

### How each is pinned

| Tool | Version | Pin |
|---|---|---|
| github/codeql-action (init, analyze) | v4.38.2 | commit `2892aa5e19bbd11bc0cff5427e3b750a04d9e3c2`. The action's release names its CodeQL bundle (`defaults.json`: `codeql-bundle-v2.27.1`) and uses the runner's tool cache only for that exact version, downloading it otherwise; moving the SHA is what moves the bundle. No `tools:` is passed: the action cannot check a checksum for a bundle URL, and downloading and checking the 600 MB bundle in the job costs more than it adds over the action's own pin |
| osv-scanner | 2.6.0 | SHA-256 of `osv-scanner_linux_amd64`, from the release's `osv-scanner_SHA256SUMS` |
| Trivy | 0.74.0 | SHA-256 of `trivy_0.74.0_Linux-64bit.tar.gz`, from the release's checksums file. The misconfiguration checks are the ones compiled into that release (`--skip-check-update`); the `trivy-action` is not used |
| actionlint | 1.7.12 | SHA-256 of `actionlint_1.7.12_linux_amd64.tar.gz`, from the release's checksums file |
| zizmor | 1.30.1 | SHA-256 of the PyPI manylinux x86_64 wheel (the release publishes no checksum file; PyPI's hash is the published one), unpacked with `unzip`, no pip |
| shellcheck | 0.9.0-1 | Ubuntu 24.04's package, by exact version |

The pins live in one place, `tools/security/tools.sh`, which fetches into
`tools/security/.tools`, checks each SHA-256 before unpacking, and serves the same binaries to
CI and to a developer's machine. The advisory data is not pinned, deliberately: osv-scanner
asks api.osv.dev and Trivy downloads its database at scan time, since a scan against last
month's advisories would miss what the check is for.

### The vendored C and C++

`tools/security/cpp-deps.py` reads each `URL`/`URL_HASH` pair from `cmake/Dependencies.cmake`,
looks the tarball up in its table by SHA-256, and writes osv-scanner's own lock-file format
with the upstream repository and the commit its tag names. OSV records C and C++
vulnerabilities as commit ranges of the upstream repository, so the commit is what matches. A
tarball the table lacks, or with a different hash, fails the script, so a dependency update
has to update its row.

| Tarball | Repository | Tag | Commit |
|---|---|---|---|
| `llhttp-9.2.1.tar.gz` | nodejs/llhttp | v9.2.1 (the tarball is `release/v9.2.1`, the generated C of that tag) | `b0b279fb5a617ab3bc2fc11c5f8bd937aac687c1` |
| `googletest-1.15.2.tar.gz` | google/googletest | v1.15.2 | `b514bdc898e2951020cbdca1304b75f5950d1f59` |
| `srt-1.5.4.tar.gz` | Haivision/srt | v1.5.4 | `a8c6b65520f814c5bd8f801be48c33ceece7c4a6` |

OpenSSL, libcurl, libpq and liburing come from Ubuntu's packages, in the images at the
snapshot's versions; the image scan covers them. Rust's toolchain is pinned by
`rust-toolchain.toml` and the crates by `Cargo.lock`, which osv-scanner reads.

### Allowlists, and how they expire

| File | Scanner | An entry needs |
|---|---|---|
| `tools/security/osv-scanner.toml` | osv-scanner | `id`, `reason`, `ignoreUntil` |
| `tools/security/trivyignore.yaml` | Trivy config and image | `id`, `statement`, `expired_at`, and `paths` or `purls` |
| `tools/security/trivy-data/registries.yaml` | Trivy KSV-0125 | the registries the manifests may name, each with why |
| `.github/zizmor.yml` | zizmor | a disabled audit, with why and when to revisit |

`tools/security/check-allowlists.py` runs before each scan and fails an entry without a
reason, without an expiry, with an expiry more than a year out, or (Trivy) without a path or
package to scope it to. Past its date an entry stops applying, in the scanner itself, and the
finding fails its job again until someone looks at it afresh. The Trivy config scan renders the
sandbox's kustomizations one resource to a file so that an entry can name one resource, not a
whole rendering.

### cppcheck

Run once, 2.13.0 (Ubuntu 24.04's), `--enable=warning,performance,portability`, over the 175
first-party units of the ci build: 27 findings, none a defect.

| Findings | Where | Why they add nothing |
|---|---|---|
| 6 `arithOperationsOnVoidPointer` | `net/src/uring_reactor.cpp:116, 128, 377, 396, 413, 856` | the buffers are typed; cppcheck mistyped them, and the code is `-Wpedantic -Werror` clean |
| 7 `ignoredReturnValue` | `apps/live-packager/src/stream_runner.cpp`, `recorder.cpp` | the packager's own `log`, taken for `std::log` |
| 1 `accessMoved` | `infra/sfu/livekit/src/room_service.cpp:191` | `done` is moved once |
| 1 `throwInNoexceptFunction` | `apps/chat/src/presence.cpp:547` | `pump`'s rethrow inside `on_timeout`'s `try`; clang-tidy's `bugprone-exception-escape` already proves only allocation failures leave it |
| 3 `syntaxError`, `preprocessorErrorDirective` | `infra/auth/src/base64url.cpp`, `jwks_verifier.cpp`, `infra/ffmpeg/src/seccomp_filter.hpp` | C++23 it cannot parse, and an `#error` for architectures the build never targets |
| 2 `virtualCallInConstructor` | `infra/storage/fake/src/fake_store.cpp:91`, `infra/storage/fs/src/fs_store.cpp:386` | a destructor calling its own class's `abort()`, as intended |
| 7 `passedByValue`, `uselessCallsConstructor`, `returnStdMoveLocal` | various | copies of small or moved-from values; clang-tidy's performance checks cover the ones that matter |

cppcheck is not added. Reopen if clang-tidy is dropped, or a cppcheck release parses C++23.

## Consequences

- Fixed with this decision: llhttp and srt are compiled with the same hardening flags as the
  project (`ulw_hardening_flags`, `cmake/Warnings.cmake`, applied in `Dependencies.cmake`), so
  the Clang build's servers carry IBT and SHSTK; protobuf in the call-capacity client goes
  from 6.32.1 to 6.33.5 (CVE-2026-0994); the migrate init container and the upload reaper get a
  CPU limit, and the STUNner operator and mock-auth pods name their image's user; every
  checkout sets `persist-credentials: false`; `setup-node` no longer keeps a package-manager
  cache; the clang-tidy step reads `github.base_ref` through the environment instead of
  splicing it into the script; the setup action configures ccache through its own
  configuration file instead of `$GITHUB_ENV`.
- zizmor's `self-repository` audit is disabled until actionlint accepts `uses: $/...`, which
  its newest release rejects; `.github/zizmor.yml` says when to revisit.
- CodeQL adds one GCC build of the servers per pull request, in parallel with the rest.
- The first nightly image scan may find HIGH vulnerabilities in the worker's ffmpeg whose only
  fixes are in Ubuntu Pro's ESM archive; those need either an allowlist entry, with a
  statement and an expiry, or a different ffmpeg, decided then.
- Changing a vendored tarball means updating `tools/security/cpp-deps.py` as well as
  `third_party/README.md`.
- Upgrading a scanner is a change to `tools/security/tools.sh` (version, SHA-256, URL), and for
  CodeQL to the action's SHA; both are reviewed like any dependency.

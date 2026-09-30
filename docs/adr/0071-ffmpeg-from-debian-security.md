# 0071. The worker's ffmpeg comes from Debian 13's security archive

Status: Accepted
Date: 2026-09-30

## Context

The worker image ran Ubuntu 24.04's ffmpeg, `7:6.1.1-3ubuntu5` (ADR-0030). ffmpeg is in
Ubuntu's universe component, which Canonical's free security updates do not cover: its fixes
land in the Ubuntu Pro ESM archive, a paid subscription, and the free archive keeps the
version the release shipped with. ffmpeg is the one component of the platform that parses
untrusted bytes by design (every upload, every live publish), which is why it runs sandboxed
(ADR-0025, ADR-0032) under a syscall allowlist (ADR-0048); a sandbox narrows what an exploit
can reach, but it does not replace fixing the decoder.

Debian 13 (trixie) carries ffmpeg 7.1 in main, which the Debian security team supports: its
fixes ship as DSAs on security.debian.org, free. Trivy's advisory database (built from
Debian's and Ubuntu's security trackers), read on 2026-09-30:

| | Debian 13, ffmpeg `7:7.1.5-0+deb13u1` (DSA-6361-1) | Ubuntu 24.04, ffmpeg `7:6.1.1-3ubuntu5` |
|---|---|---|
| Open CVEs against the source package | 31 | 57 |
| NVD CRITICAL among them | 0 | 1 |
| NVD HIGH among them | 4 | 5 |

@SCAN@

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Stay on Ubuntu, buy Ubuntu Pro for ESM | Same distribution as CI and the gateway; ESM carries ffmpeg's fixes | Rejected: a paid subscription, a token in the image build, and it keeps ffmpeg 6.1 |
| Build ffmpeg from source in the image | Newest release, only the codecs we use, smallest image | Rejected for now: we would become ffmpeg's security team, tracking upstream's point releases and every library under it (x264, dav1d, libvpx...) ourselves, with a pinned source and build of each |
| A static ffmpeg build from a third party | One file, no dependency tree | Rejected: a binary built by someone else, whose security fixes arrive when they rebuild, with no signed archive behind it |
| Stay on Ubuntu and allowlist ffmpeg's CVEs in the scanner | No change at all | Rejected: the CVEs stay open; the list would only stop us seeing them |
| The worker on Debian 13, ffmpeg from trixie-security at a pinned snapshot | Free, signed security updates from a team that fixes ffmpeg; the same pinning discipline as ADR-0030 | Accepted |

## Decision

- The `worker` target of `deploy/docker/Dockerfile` starts from `debian:trixie-slim` pinned by
  digest. Every package it installs is named with its exact version and comes from one
  snapshot.debian.org timestamp (`DEBIAN_SNAPSHOT`), for both the main archive (`trixie`,
  `trixie-updates`) and the security archive (`trixie-security`), through
  `deploy/docker/apt-install-debian.sh`: apt checks each index against the snapshot's signed
  Release file and the archive keys the base image ships.
@PINS@
- `transcode_worker` and `ulw_sandbox` are built in a trixie stage (`worker-build`) from the
  same snapshot, with trixie's GCC 14, CMake and -dev packages, so they link the glibc,
  libstdc++, OpenSSL, libcurl, libpq and liburing they were compiled against, as ADR-0030
  asks. Running Ubuntu-built binaries on trixie happens to load (trixie's glibc 2.41 and
  libstdc++ are newer than noble's 2.39), but OpenSSL (3.0 against 3.5), libpq (16 against 17)
  and libcurl would each be a version the binary was never built or tested against, and the
  first library whose sonames or symbols differ would break only at run time.
- The gateway image (`gateway`, with `ulw_migrate` and `ulw_reaper`) stays on Ubuntu 24.04 as
  ADR-0030 has it: it carries no ffmpeg, and its libraries are covered by Ubuntu's free
  updates in main. CI and the other e2e jobs keep the runner's Ubuntu ffmpeg; the e2e sandbox
  job builds and runs the Debian worker image.
@SECCOMP@

## Updates

- A DSA for ffmpeg (or for any package in the worker image: glibc, OpenSSL, libcurl, libpq,
  and ffmpeg's codec libraries) is picked up by moving `DEBIAN_SNAPSHOT` to a timestamp after
  the DSA's upload reached snapshot.debian.org and bumping the pinned versions that changed
  (RUNBOOK.md, "Updating the worker's ffmpeg"). The candidates at a timestamp are what
  `apt-install-debian.sh --policy PACKAGE...` prints in a `debian:trixie-slim` container.
- Who and how often: the platform's on-call engineer, who already owns the image pins. Every
  Monday they read the debian-security-announce list (or the tracker's page for each pinned
  source package) and bump within two working days of a DSA for ffmpeg, OpenSSL or glibc,
  and at least monthly otherwise, so the other libraries follow. A bump that changes ffmpeg's
  minor version re-runs `tools/trace-ffmpeg-syscalls.sh` in the image first (ADR-0048).

## Consequences

- Two distributions ship: the gateway on Ubuntu, the worker on Debian. ADR-0030's rejection of
  a Debian runtime was for a second distribution no test had seen; the worker's is now built
  and exercised in CI (the e2e sandbox job builds this image and runs the VOD flow through
  it), and its seccomp list was traced against it. Unit and integration jobs still run on the
  runner's Ubuntu ffmpeg 6.1, so an ffmpeg behaviour change shows first in the sandbox job.
- ffmpeg moves from 6.1 to 7.1. The arguments the worker, the live packager and the
  recording remux pass behave the same (the checks are under Decision).
- The image build depends on snapshot.debian.org as well as snapshot.ubuntu.com. It throttles
  under load, which apt's retries absorb; its Release files are past their Valid-Until, so that
  check is off and the pinned timestamp decides the archive state.
- Trixie's security support runs to mid-2028, then LTS to 2030. Moving to Debian 14 is a
  deliberate change like this one.

# 0030. Container images build and run on Ubuntu 24.04 from a dated snapshot

Status: Accepted
Date: 2026-09-29

## Context

The gateway and worker ship as container images (deploy/docker/Dockerfile) that Woodpecker
builds on every push. The binaries link OpenSSL 3, libcurl, libpq and liburing dynamically, and
the worker runs the distribution's ffmpeg (ADR-0025). Every fetched dependency must be pinned
by version and hash. CI and the development machines build on Ubuntu 24.04 with GCC 14.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Distroless runtime | Smallest image, no shell or package manager | Rejected: it carries no libcurl, libpq or liburing, so they would be copied in by hand with their own dependency trees, and the worker needs ffmpeg's hundreds of libraries |
| Debian slim runtime over a Debian build stage | A common small base | Rejected: a second distribution beside the one CI tests on, so the image would run against library versions no test has seen |
| Ubuntu 24.04 for build and runtime, packages at pinned versions from the live archive | Same libraries as CI; simple | Rejected: the archive keeps only the newest version of an updated package, so a pin breaks the build as soon as a security update lands |
| Ubuntu 24.04 for build and runtime, packages from snapshot.ubuntu.com at a fixed timestamp | Same libraries as CI; a rebuild months later installs the same bytes, each checked by apt against the snapshot's signed Release file | Accepted |

## Decision

- Both stages start from `ubuntu:24.04` pinned by digest. Packages are installed by
  `deploy/docker/apt-install.sh`, which uses apt's `--snapshot` with the timestamp in the
  Dockerfile and exact versions for every package named.
- The runtime images hold only the shared libraries, `ca-certificates`, and for the worker
  `ffmpeg`; no compiler, no apt lists. They run as uid 10001 with a read-only root filesystem.
- The gateway image also carries `ulw_migrate`, so the schema and the server always come from
  one build (ADR-0031).
- A build behind a TLS-inspecting proxy passes its CA as the build secret `ca-bundle`; it is
  mounted for the apt steps only and never lands in a layer.

## Consequences

- Moving to newer packages, security fixes included, is a deliberate change of the snapshot
  timestamp and the version pins, reviewed like any dependency bump. Monitor Ubuntu's security
  notices for the pinned versions of OpenSSL, libcurl, libpq and ffmpeg.
- The worker image is large (about 200 MB compressed) because Ubuntu's ffmpeg links every
  codec and device library. A static ffmpeg build would shrink it but would need its own
  pinned source and build.
- The snapshot service is a build-time dependency; it answers 503 under load, which the apt
  retries absorb. Reopen if it becomes unreliable, or if the platform moves off Ubuntu.
- The worker image's runtime moved to Debian 13 with ffmpeg from Debian's security archive,
  pinned the same way from snapshot.debian.org (ADR-0074). Its binaries are still built here,
  on Ubuntu 24.04, for their CET marks; the gateway image stays as above.

# 0074. The worker's ffmpeg comes from Debian 13's security archive

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

Trivy 0.74.0 (the scanner PR #64 pins) on the two worker images, 2026-09-30, counting distinct
CVEs:

| | before: Ubuntu 24.04 image | after: Debian 13 image |
|---|---|---|
| In ffmpeg's own packages (ffmpeg, libav*, libsw*, libpostproc) | 57 | 31 |
| ... of which NVD rates HIGH or CRITICAL | 6 | 4 |
| Whole image, NVD CRITICAL | 2 | 2 |
| Whole image, NVD HIGH | 15 | 39 |
| Whole image, Trivy CRITICAL | 0 | 1 (CVE-2026-6653, libxml2, no fix yet) |
| Whole image, Trivy HIGH | 1 | 50 |
| Trivy HIGH or CRITICAL with a fixed version available | 1 (CVE-2026-84782, OpenSSL) | 0 |

The whole-image rows go up, and the reason matters for reading them. Debian's `ffmpeg` package
pulls in 207 packages against Ubuntu's smaller set (libavdevice brings SDL, X11, Mesa and
LLVM; libavfilter brings librsvg and libxml2), so there is more to scan; and Trivy rates an
Ubuntu package by Ubuntu's priority, which puts most of them at medium, while for Debian it
falls back to other vendors' ratings where Debian gives none. Of the 50 Trivy HIGHs after, 16
are ffmpeg's own and the rest are util-linux (4), libxml2 (7), libcjson1 (5), libcurl (4),
libexpat (4) and single ones in X11, ncurses, perl-base, systemd's libraries, libtiff and
librsvg; none has a fix in trixie yet, so the gate that fails on fixable HIGHs passes. The old
image fails it on OpenSSL. What was asked for is the row that decides this ADR: ffmpeg's open
CVEs drop from 57 to 31, none CRITICAL, and the ones left get fixed for free as DSAs land.

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
  Release file and the archive keys the base image ships. The snapshot is fetched over https:
  its Release files are past their Valid-Until by design, so that check is off, and plain http
  would let anyone on the path serve an older signed Release and roll back every unpinned
  package with it. The base image has no CA bundle, so ca-certificates is bootstrapped from
  the live archive (http, but signed and with Valid-Until checked) and then pinned back to the
  snapshot's version; a build behind a TLS-inspecting proxy passes its CA as the `ca-bundle`
  build secret instead.
- The packages the base image already holds (libc6, util-linux, perl-base, ncurses,
  libsystemd0 and the rest) are upgraded to the snapshot's versions before the pinned install,
  since an install upgrades only what it names. A snapshot bump therefore moves them too, and
  the base digest only decides where that upgrade starts.
- Pinned at `DEBIAN_SNAPSHOT=20260930T100000Z`: `ffmpeg=7:7.1.5-0+deb13u1` (DSA-6361-1, in
  debian-security since 20260622T192543Z), `openssl` and `libssl3t64` `3.5.7-1~deb13u3` (in
  debian-security since 20260930T060347Z, which fixes the two OpenSSL HIGHs deb13u2 had),
  `libcurl4t64=8.14.1-2+deb13u5`, `libpq5=17.11-0+deb13u1`, `liburing2=2.9-1`,
  `ca-certificates=20250419`; to build, `g++-14=14.2.0-19`, `cmake=3.31.6-2`,
  `ninja-build=1.12.1-1`, `pkgconf=1.8.1-4` and the matching -dev packages.
- `transcode_worker` and `ulw_sandbox` are built in a trixie stage (`worker-build`) from the
  same snapshot, with trixie's GCC 14, CMake and -dev packages, so they link the glibc,
  libstdc++, OpenSSL, libcurl, libpq and liburing they were compiled against, as ADR-0030
  asks. Ubuntu-built binaries would probably load on trixie (glibc 2.41 is newer than noble's
  2.39, both carry GCC 14.2's libstdc++, and the sonames match), but OpenSSL (3.0 against 3.5),
  libpq (16 against 17) and libcurl (8.5 against 8.14) would each be a version the binary was
  never compiled against, and a difference would show only at run time. The trixie build
  already found one: libcurl 8.14 declares `CURL_HTTP_VERSION_1_1` as a long, which made a cast
  in `infra/curl` fail `-Wuseless-cast`; the code now compiles against both. Two builder stages
  cost one more compile of the worker's two targets per image build.
- The gateway image (`gateway`, with `ulw_migrate` and `ulw_reaper`) stays on Ubuntu 24.04 as
  ADR-0030 has it: it carries no ffmpeg, and its libraries are covered by Ubuntu's free
  updates in main. CI and the other e2e jobs keep the runner's Ubuntu ffmpeg; the e2e sandbox
  job builds and runs the Debian worker image.
- The syscall allowlist (ADR-0048) needs nothing new: `tools/trace-ffmpeg-syscalls.sh`, now
  also covering the worker's keyframe and decode checks, the live recording's remux, and the
  probe and transcode of HEVC, VP9, AV1 and MPEG-2 sources beside H.264, traced only names
  already allowed on 7.1.5, as root and as an ordinary user (glibc 2.41 still falls back from
  `clone3` to `clone`, which the filter checks). The ffmpeg suites and the worker's, live
  packager's and live recording's integration suites pass on 7.1.5 under the filter.
- The trixie workflow keeps that true: on every pull request that touches the Dockerfile or
  the code around ffmpeg, and nightly, it builds the `worker-build` stage, installs the image's
  ffmpeg and runs those suites, the trace and `tools/ffmpeg-address-space.sh` there. ci.yml's
  jobs run the runner's ffmpeg 6.1, on which `settle_master_bandwidth` changes nothing.
- ffmpeg 7 rewrites the master playlist when a run ends, with each variant's measured peak and
  average segment bitrate (`AVERAGE-BANDWIDTH` is new). Those differ between two runs of a job,
  and everything but the segments must not (a rerun can overwrite some of another run's keys;
  `TranscoderTest.SegmentsOfTwoRunsMixIntoARenditionThatPlays`). The transcoder writes the
  master back with the ladder's declared rates plus a tenth and no `AVERAGE-BANDWIDTH`, exactly
  what 6.1 wrote (`settle_master_bandwidth`). Verification requires it: a master whose
  BANDWIDTH is not the ladder's fails `check_master_playlist`, so a future ffmpeg whose master
  no longer settles fails the job instead of shipping a master that differs between runs.
- The live remux and both stages of the live recording's remux run with `MALLOC_ARENA_MAX=2`
  (below).
- The other arguments behave as on 6.1: the transcode and the live remux produce the same
  playlists (version 7, `EXT-X-MAP`, `EXT-X-INDEPENDENT-SEGMENTS`, fMP4 segments,
  `temp_file`'s renames), and `-analyzeduration`/`-probesize` still bound the live probe: the
  packager's late-keyframe test (ADR-0057) passes on 7.1.5.

## Updates

- A DSA for ffmpeg, or for any package in the worker image (glibc, OpenSSL, libcurl, libpq,
  ffmpeg's codec libraries, and the base image's own packages), is picked up by moving
  `DEBIAN_SNAPSHOT` to a timestamp after the DSA's upload reached snapshot.debian.org and
  bumping the pinned versions that changed; the base image's packages follow the snapshot on
  their own, and a new point release's digest is taken with the same bump (RUNBOOK.md,
  "Updating the worker's ffmpeg"). The candidates at a timestamp are what
  `apt-install-debian.sh --policy PACKAGE...` prints in a `debian:trixie-slim` container.
- Who and how often: the platform's on-call engineer, who already owns the image pins. The
  daily trigger is the nightly e2e run, whose Trivy gate fails as soon as a fixable HIGH or
  CRITICAL reaches the worker image; every Monday they also read debian-security-announce for
  what the gate does not rate HIGH. They bump within two working days of a DSA for ffmpeg,
  OpenSSL or glibc, and at least monthly otherwise, so the other libraries follow.
- A bump that changes ffmpeg's upstream version (7.1.5 to 7.1.6, not a `+deb13uN` patch)
  re-runs `tools/trace-ffmpeg-syscalls.sh` and `tools/ffmpeg-address-space.sh` in the image
  first (ADR-0048); the trixie workflow runs both on the pull request as well.

## Consequences

- Two distributions ship: the gateway on Ubuntu, the worker on Debian. ADR-0030's rejection of
  a Debian runtime was for a second distribution no test had seen; the worker's is now built
  and exercised in CI (the e2e sandbox job builds this image and runs the VOD flow through
  it, its release binaries' hardening and its Trivy gate), and the trixie workflow runs the
  ffmpeg suites on it. ci.yml's unit and integration jobs still run on the runner's Ubuntu
  ffmpeg 6.1.
- ffmpeg moves from 6.1 to 7.1. The arguments the worker, the live packager and the
  recording remux pass behave the same (the checks are under Decision).
- ffmpeg 7.1 on Debian maps more address space than 6.1 on Ubuntu: it runs every demuxer,
  filter, encoder and muxer on a thread of its own, and glibc gives each thread that allocates
  concurrently an arena reserving 64 MiB. Peak VmPeak on 4 cores, 6.1 then 7.1 with glibc's
  default arenas, against the sandbox's limits: probe 273 then 249 MiB (1 GiB); live remux 496
  then 492 MiB (1 GiB); the live recording's remux with added silence 688 then 811-923 MiB
  (1 GiB); the 1080p three-rung transcode with `-threads 4` about 1.7-1.9 GB then 2970 MiB
  (4 GiB); the decode check 1299 then 1722 MiB (4 GiB). The silence stage had a tenth of its
  budget left, and a remux that runs out loses the recording for good (ADR-0055), so the
  remuxes now run with `MALLOC_ARENA_MAX=2`: on 7.1.5 the silence stage peaks at 435 MiB and
  the live remux at 412 MiB, at the same resident size, and the 1 GiB limit stays.
  `tools/ffmpeg-address-space.sh` measures each command line under its limit and environment.
- An x264 encode at its default thread count went from 971 to 1236-1491 MiB, which is why the
  syscall filter test's source clip now gets 4 GiB; the remux it tests keeps the packager's
  1 GiB and environment.
- The image build depends on snapshot.debian.org as well as snapshot.ubuntu.com, and on
  deb.debian.org for the bootstrap. snapshot.debian.org throttles under load, which apt's
  retries absorb, and a failed index fetch fails the build rather than installing from what
  apt had before.
- Trixie's security support runs to mid-2028, then LTS to 2030. Moving to Debian 14 is a
  deliberate change like this one.

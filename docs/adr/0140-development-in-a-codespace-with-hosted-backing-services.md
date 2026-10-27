# 0140. Development runs in a codespace, with hosted backing services optional

Status: Accepted
Date: 2026-10-27

## Context

The platform builds and runs only on Linux: io_uring and epoll (ADR-0010), namespaces and
seccomp around ffmpeg (ADR-0025, ADR-0048), GCC 14 or Clang 19 against Linux libraries. A
developer on a Windows machine that holds nothing but the code could neither build nor test it,
nor run the backing services the suites need (Postgres, MinIO, LiveKit; deploy/local/compose.yaml).
Every piece of the environment therefore has to be somewhere else, on free tiers, and described
in one place.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A dev container, run as a GitHub Codespace: CI's toolchain, Docker inside for compose.yaml and kind, the local services on persistent volumes; Neon, R2 and LiveKit Cloud as optional hosted services through Codespaces secrets | Nothing on the developer's machine but a browser; the same Ubuntu 24.04 and toolchain as CI; every suite runs unchanged; free allowance on a personal account; the same container runs under Docker Desktop too | 120 core-hours a month (30 hours on 4 cores); forwarded ports carry no UDP, so calls from the developer's own browser need LiveKit Cloud and live streaming stays inside the codespace | Accepted |
| WSL 2 on the Windows machine | No quota; local speed | Installs a Linux, Docker and the toolchain on the machine, which the requirement rules out | Rejected |
| A free cloud VM (always-free tiers), reached over SSH | No hour quota | One more machine to provision, patch and secure by hand; free shapes are small or ARM, and the toolchain is set up outside the repository | Rejected |
| Only hosted services (Neon, R2, LiveKit Cloud), building elsewhere | Data outlives any machine | Still needs a Linux machine to build and run on; the suites need a local Postgres they can pause and flood with 400 connections | Rejected as the whole answer; kept as the optional half of the first |

## Decision

- `.devcontainer/` defines the environment: the base image and features pinned by digest
  (ADR-0072), the setup action's package list plus psql, coturn and GStreamer, Docker in Docker
  (privileged, which leaves io_uring and user namespaces to the kernel), Node 22, Go 1.24 and
  the Rust release the OpenMLS bridge pins. It asks for 4 cores.
- post-start.sh makes CI's two sysctls and starts Postgres and MinIO with
  `.devcontainer/compose.persist.yaml` laid over compose.yaml, which moves their data to named
  volumes on the codespace's disk.
- `tools/dev/` runs the platform from the build tree: `up.sh` (migrate, then gateway, worker
  and chat, each with only the settings named for it), `down.sh`, `token.sh` (a dev key kept in
  `.dev/`, ADR-0088), `smoke.sh` (one video end to end), `services.sh`, `doctor.sh`, `build.sh`.
- `ULW_DEV_BACKEND`, or `ULW_DEV_DATABASE`, `ULW_DEV_STORAGE` and `ULW_DEV_CALLS` one by one,
  choose local services or hosted ones: Neon, R2 and LiveKit Cloud, read from `ULW_CLOUD_*`
  Codespaces secrets (or a git-ignored `.env`). The test suites always use the local services.
- docs/development/cloud-dev.md is the guide: what is hosted where and its allowance, the
  secrets, each provider's setup, the commands, and what reaches the developer's own browser.

## Consequences

- A contributor needs a GitHub account and a browser; the first codespace takes 10 to 15
  minutes to build, later starts under a minute.
- The container's package list duplicates the setup action's; a package added there for the
  build or the tests has to be added in `.devcontainer/Dockerfile` too.
- Playback from the developer's own browser needs R2 (the local MinIO's presigned URLs name a
  loopback address); calls from it need LiveKit Cloud; live streaming is tested only inside the
  codespace.
- Free allowances bound the work: a month's compute is 30 hours on 4 cores unless the developer
  adds a spending limit, and Neon's free database holds 0.5 GB.

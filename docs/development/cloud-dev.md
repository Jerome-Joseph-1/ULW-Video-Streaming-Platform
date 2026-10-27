# Developing in the cloud (from Windows, or any machine)

Everything this repository needs to build, test and run lives in the cloud, on free tiers. The
machine you sit at needs **only a browser** (or VS Code, if you prefer it). Nothing is installed
on Windows: no compiler, no Docker, no WSL, no database.

Why not native Windows: the platform is Linux-only by design. The reactor is io_uring with an
epoll fallback (ADR-0010), the worker sandboxes ffmpeg with Linux namespaces and seccomp
(ADR-0025, ADR-0048), and the binaries are built with GCC 14 or Clang 19 against Linux
libraries. None of it compiles on Windows, so the Linux machine moves to the cloud instead
(ADR-0140).

## What is hosted where

| What | Where it lives | Free allowance (check the provider's page; these change) | Needed? |
|---|---|---|---|
| Code, history, CI | GitHub (this repository) | Public repository: unlimited Actions minutes | Yes |
| The Linux machine: toolchain, builds, tests, the running services | **GitHub Codespaces**, from [`.devcontainer/`](../../.devcontainer/devcontainer.json) | Personal accounts: 120 core-hours and 15 GB-month storage a month. The 4-core machine this repository asks for gives 30 hours a month | Yes |
| Local Postgres 16 and MinIO (S3) | Containers inside the codespace, data on volumes on its disk | Part of the codespace | Yes (the test suites use them) |
| Secrets (database URL, storage keys, LiveKit keys) | **Codespaces secrets** in your GitHub settings | Free | Only for the hosted services below |
| Persistent Postgres, reachable from anywhere | **Neon** (serverless Postgres 16) | 0.5 GB storage, 100 compute-hours a month per project, scales to zero after 5 minutes idle | Optional |
| Persistent object storage, reachable from anywhere | **Cloudflare R2**, the store production uses (ADR-0011) | 10 GB-month, 1M writes and 10M reads a month, no egress fees. Cloudflare may ask for a payment method to turn R2 on | Optional |
| Calls (WebRTC SFU and TURN) reachable from your own browser | **LiveKit Cloud** | Build plan: 5,000 WebRTC participant-minutes a month, a hard cap, no overage | Optional |
| Identity provider | None: a dev signing key made in the codespace (`tools/dev/token.sh`, ADR-0088) | n/a | n/a |

**What has to be hosted at the minimum** is the codespace. With it alone you can build, run every
test suite (unit, integration, conformance, E2E, calls) and run the whole platform, all inside
the codespace. Neon, R2 and LiveKit Cloud are for data that should outlive any one codespace, and
for reaching the running platform from a browser on your own machine (see [What works from
your browser](#what-works-from-your-own-browser)).

**What is not in the cloud and does not need to be:** your editor settings and nothing else.
Anything you kept on a Linux machine outside git (a `.env`, keys, test videos) has to move: a
setting or secret into a [Codespaces secret](#3-add-the-secrets), a file into R2 or the repository.

## 1. Create the codespace (once)

1. On GitHub, open this repository, then **Code → Codespaces → ⋯ → New with options**.
2. Branch: the one you work on. Machine type: **4-core** (the configuration asks for it; a full
   build is about 650 C++ files). Region: the one nearest you.
3. **Create codespace.** The first start builds the container image (toolchain, ffmpeg,
   GStreamer, Docker, Node 22, Go 1.24, Rust 1.94.1), fetches the Rust crates and starts Postgres
   and MinIO. Allow 10 to 15 minutes. Later starts take under a minute.

It opens VS Code in the browser. To use VS Code installed on Windows instead, install the
*GitHub Codespaces* extension there and choose **Open in VS Code Desktop**: the code and every
process still run in the cloud.

Then, in the codespace's terminal:

```sh
tools/dev/doctor.sh          # checks the toolchain, kernel features, services, and any secrets
tools/dev/build.sh           # cmake --preset dev && cmake --build --preset dev (first time: ~10-20 min)
```

## 2. Everyday commands

All of these run in the codespace's terminal.

| Task | Command |
|---|---|
| Build (dev preset, every target) | `tools/dev/build.sh` |
| Build only some targets | `tools/dev/build.sh dev gateway_server chat_server` |
| Build as CI's pull request lane does (warnings are errors) | `tools/dev/build.sh ci` |
| Unit tests | `ctest --preset dev -L unit` |
| Every C++ test (unit, integration against the local Postgres, ...) | `ctest --preset dev` |
| Browser E2E (upload, transcode, hls.js playback in headless Chrome) | `tests/e2e/run.sh build/dev` |
| Call suite (LiveKit, headless Chrome peers) | `tools/dev/services.sh calls && tests/call/run.sh build/dev` |
| Checks CI runs on every pull request | `tools/check-format.sh`, `tools/check-boundaries.sh`, `python3 tools/check-docs.py`, `tools/run-clang-tidy.sh build/dev <files>` |
| The whole platform in a kind cluster behind Envoy | `make e2e-up`, `make e2e-test`, `make e2e-down` |
| Start the platform (gateway, worker, chat) | `tools/dev/up.sh` |
| A token to call it with | `tools/dev/token.sh alice` |
| One video through it, end to end | `tools/dev/smoke.sh` |
| Follow its logs | `tools/dev/logs.sh` (or `tools/dev/logs.sh gateway`) |
| Stop it | `tools/dev/down.sh` |
| Local Postgres and MinIO | `tools/dev/services.sh up` (also `status`, `down`, `reset`) |
| Check the environment | `tools/dev/doctor.sh` |

The README's sections on presets, checks and security scanners apply unchanged: the codespace
is an Ubuntu 24.04 machine with CI's toolchain.

### Running the platform

`tools/dev/up.sh` applies the schema (`ulw_migrate`) and starts, in the background:

| Service | Port | Reached from the codespace | Reached from your browser |
|---|---|---|---|
| `gateway_server` | 8080 | `http://127.0.0.1:8080` | the forwarded URL `up.sh` prints, `https://<codespace>-8080.app.github.dev` |
| `transcode_worker` | none | | |
| `chat_server` | 8081 (client), 8082 (node channel) | `ws://127.0.0.1:8081/rt` | `wss://<codespace>-8081.app.github.dev/rt` |

They trust tokens signed with a dev key that `up.sh` makes once in `.dev/` (ignored by git) and
keeps: `tools/dev/token.sh <user>` mints one, valid for a day.

```sh
tools/dev/up.sh
tools/dev/smoke.sh                       # upload, transcode, playlist, segment: prints "smoke: ok"
TOKEN=$(tools/dev/token.sh alice)
curl -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/videos
tools/dev/down.sh
```

The API is in [`docs/integration/`](../integration/README.md).

### Choosing the backing services

By default everything runs against the codespace's own Postgres and MinIO. Set
`ULW_DEV_BACKEND=cloud` to use Neon and R2 instead, or pick each one:

| Variable | `local` (default) | `cloud` |
|---|---|---|
| `ULW_DEV_BACKEND` | both below local | both below cloud |
| `ULW_DEV_DATABASE` | local Postgres, database `ulw_dev` | `ULW_CLOUD_DATABASE_URL` (Neon) |
| `ULW_DEV_STORAGE` | local MinIO, bucket `ulw-dev` | R2, from the `ULW_CLOUD_R2_*` secrets |
| `ULW_DEV_CALLS` | `off` (default): chat without calls; `local`: the codespace's LiveKit | LiveKit Cloud, from the `ULW_CLOUD_LIVEKIT_*` secrets |

```sh
ULW_DEV_BACKEND=cloud tools/dev/up.sh
ULW_DEV_BACKEND=cloud ULW_DEV_CALLS=cloud tools/dev/up.sh
ULW_DEV_DATABASE=cloud tools/dev/up.sh          # Neon, with the local MinIO
```

The test suites always use the local services: they create and drop scratch databases and
buckets, pause the Postgres container, and open 400 connections at once, none of which belongs
on a free hosted database.

### What works from your own browser

The codespace forwards ports 8080 and 8081 over HTTPS, privately: only you, signed in to
GitHub, can reach them. From your machine:

- **API calls** (Postman, curl, a page you are developing): use the forwarded gateway URL with
  `Authorization: Bearer <token>`. A tool that is not your signed-in browser cannot pass GitHub's
  sign-in; for that, set the port's visibility to *Public* in VS Code's **Ports** panel while you
  test (the gateway still requires a valid token), and back to *Private* after.
- **Playback** needs the segment URLs the gateway presigns to be reachable from your browser.
  With the local MinIO they point at `127.0.0.1:9000` inside the codespace, which your browser
  cannot reach; with `ULW_DEV_STORAGE=cloud` they point at R2, which it can. So test playback
  from your own browser against R2 (with the CORS rule below), and inside the codespace with the
  E2E suite, which runs a real browser there.
- **Calls** need WebRTC media (UDP) to reach the SFU, which the forwarder does not carry. Use
  `ULW_DEV_CALLS=cloud`: your browser then talks to LiveKit Cloud directly and `chat_server` in
  the codespace only signs tickets. The local LiveKit serves the call suite's browsers inside the
  codespace.
- **Live streaming** (WHIP ingest, the recorder, the SRT packager; ADR-0053) runs only inside
  the codespace (`tools/dev/services.sh calls`, then `tests/e2e/live-run.sh` and
  `tests/e2e/live-publish-run.sh`): LiveKit Cloud's recorder would have to reach a packager in
  the codespace over SRT, which nothing forwards.

`up.sh` allows both forwarded origins in `ULW_ALLOWED_ORIGINS`; add more, comma-separated, with
`ULW_DEV_EXTRA_ORIGINS=https://...`.

## 3. Add the secrets

Only for the hosted services you use. Add each at **GitHub → Settings → Codespaces → Secrets →
New secret**, and under *Repository access* choose this repository. A running codespace sees a
new or changed secret after it restarts (**⋯ → Stop codespace**, then open it again).

| Secret | Value | Used by |
|---|---|---|
| `ULW_CLOUD_DATABASE_URL` | Neon's **direct** connection string (not `-pooler`), with `?sslmode=require` | `ULW_DEV_DATABASE=cloud` |
| `ULW_CLOUD_R2_ACCOUNT_ID` | Your Cloudflare account id (32 hex characters) | `ULW_DEV_STORAGE=cloud` |
| `ULW_CLOUD_R2_BUCKET` | The bucket's name, e.g. `ulw-dev` | |
| `ULW_CLOUD_R2_ACCESS_KEY_ID` | The R2 API token's access key id | |
| `ULW_CLOUD_R2_SECRET_ACCESS_KEY` | The R2 API token's secret access key | |
| `ULW_CLOUD_LIVEKIT_URL` | The project's URL, `wss://<project>.livekit.cloud` | `ULW_DEV_CALLS=cloud` |
| `ULW_CLOUD_LIVEKIT_API_KEY` | An API key of that project | |
| `ULW_CLOUD_LIVEKIT_API_SECRET` | Its secret | |

The scripts hand each service only the settings it needs (`env -i` in `tools/dev/up.sh`), never
the codespace's whole environment. Off Codespaces, the same names can go in a `.env` file at
the repository's root, which git ignores and `tools/dev/lib.sh` reads.

`tools/dev/doctor.sh` tells you which secrets are set and whether Neon and R2 answer with them.

## 4. Set up the hosted services (optional, once each)

### Neon (Postgres)

1. Sign up at [neon.com](https://neon.com) (GitHub sign-in works); no card is needed.
2. Create a project: Postgres **16**, the region nearest your codespace's.
3. **Connect** → choose the database, turn **Connection pooling off**, and copy the string. It
   looks like `postgresql://<user>:<password>@ep-xxxx.<region>.aws.neon.tech/neondb?sslmode=require`.
   The direct connection matters: the gateway holds session-level advisory locks per upload
   (ADR-0065), which a transaction-pooling proxy would break.
4. Save it as `ULW_CLOUD_DATABASE_URL`. `tools/dev/up.sh` applies the schema on every start.

The free compute suspends after 5 minutes idle; the first query after that waits a second or
so while it wakes.

### Cloudflare R2 (object storage)

1. Sign up at [dash.cloudflare.com](https://dash.cloudflare.com); open **R2 Object Storage** and
   enable it (Cloudflare may ask for a payment method; usage inside the free allowance is not
   charged).
2. **Create bucket**: name `ulw-dev`, location automatic, Standard storage class (the free
   allowance covers only Standard).
3. **Manage API tokens → Create API token**: permission *Object Read & Write*, applied to that
   bucket only. Copy the **Access Key ID** and **Secret Access Key** (shown once) and your
   **account id** (on the R2 overview page).
4. In the bucket's **Settings → CORS policy**, add the rule ADR-0028 describes, with the
   forwarded gateway origin `tools/dev/up.sh` prints (and any page origin you develop on):

   ```json
   [{
     "AllowedOrigins": ["https://<codespace>-8080.app.github.dev"],
     "AllowedMethods": ["GET", "HEAD"],
     "AllowedHeaders": ["Range", "If-None-Match"],
     "ExposeHeaders": ["Content-Range", "Content-Length", "ETag"],
     "MaxAgeSeconds": 3600
   }]
   ```

5. In **Settings → Object lifecycle rules**, add *Abort incomplete multipart uploads* after 7
   days for prefix `videos/` (deploy/kubernetes/RUNBOOK.md step 3a), so an abandoned upload's
   parts do not count against the 10 GB.
6. Save the four `ULW_CLOUD_R2_*` secrets.

### LiveKit Cloud (calls)

1. Sign up at [cloud.livekit.io](https://cloud.livekit.io); the Build plan is free and stops at
   its quota instead of charging.
2. Create a project. Under **Settings → Keys**, create an API key; copy the key, its secret and
   the project URL (`wss://...livekit.cloud`).
3. Save the three `ULW_CLOUD_LIVEKIT_*` secrets and start with `ULW_DEV_CALLS=cloud`.

LiveKit Cloud runs its own TURN, so nothing else is needed for calls between browsers anywhere.

## 5. Keeping inside the free allowances

- **Stop the codespace when you are done** (**⋯ → Stop codespace**, or let it stop itself after
  30 idle minutes, the default). A stopped codespace uses no compute hours; its disk (build tree,
  ccache, the local services' data) is kept and counts against the 15 GB-month storage.
- Keep **one** codespace. Each one's disk counts toward storage; delete ones you no longer use
  at [github.com/codespaces](https://github.com/codespaces). Push your work first: deleting a
  codespace deletes everything in it that is not pushed, and GitHub deletes a codespace left
  stopped for 30 days (configurable in your Codespaces settings).
- Do not turn on prebuilds: they are billed as storage and Actions minutes.
- The free allowance stops Codespaces for the rest of the month when it runs out, unless you
  have added a payment method and a spending limit above zero. GitHub e-mails you at 75%, 90%
  and 100%; usage is under **Settings → Billing**.
- `tools/dev/services.sh reset` deletes the local Postgres and MinIO data; `rm -rf build/` frees
  the build trees (a few GB each).

## Data: where it lives and how long

| Data | Where | Survives |
|---|---|---|
| Code | GitHub, once pushed | Everything |
| Uncommitted work, build trees, ccache, `.dev/` (dev key, logs) | The codespace's disk | Stops and restarts; not deleting the codespace |
| Local Postgres and MinIO data | Docker volumes on the codespace's disk (`.devcontainer/compose.persist.yaml`) | Stops, restarts, `services.sh down`; not `services.sh reset` or deleting the codespace |
| Neon database, R2 bucket, LiveKit project | The providers | Independent of any codespace |
| Secrets | GitHub's Codespaces secrets | Independent of any codespace |

## Troubleshooting

- **`tools/dev/doctor.sh` says io_uring is not usable**: the services and tests fall back to
  epoll with `ULW_REACTOR=epoll`; the io_uring-specific tests fail. The container runs
  privileged (the docker-in-docker feature), which normally leaves io_uring available.
- **The worker exits at start, or sandbox tests skip**: user namespaces are refused.
  `sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0` (post-start.sh tries this at
  every start).
- **`ulw-pg` or `ulw-minio` not healthy**: `tools/dev/services.sh up`; if Docker itself is down,
  rebuild the container (**Ctrl+Shift+P → Codespaces: Rebuild Container**).
- **Postgres answers "too many clients" against Neon**: you pointed a test suite at it; the
  suites belong on the local Postgres.
- **R2 answers 403 in the doctor**: the token is scoped to another bucket, or the account id is
  wrong. A browser's 403 on a segment, with the API working, is the CORS rule's origin.
- **A secret is missing although you added it**: the codespace sees secrets from its start;
  stop and start it, and check the secret's repository access includes this repository.
- **Ports 8080/8081 are taken**: `tools/dev/down.sh`, or find the holder with `ss -ltnp`.

## Without Codespaces

The same container works anywhere Dev Containers do: VS Code with the *Dev Containers*
extension and Docker Desktop (on Windows, Docker Desktop's WSL 2 backend), or any Linux machine
with Docker, by **Dev Containers: Reopen in Container**. Everything above applies, with
`127.0.0.1` in place of the forwarded URLs and a `.env` file in place of Codespaces secrets.

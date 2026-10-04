# Local laptop demo

The whole platform on one machine with one command: upload and playback, chat with an
end-to-end encrypted room, 1:1 calls that ring, group calls, and live streaming whose recording
becomes a video. Everything runs in Docker; the page is plain HTML and JavaScript on
http://localhost:8080. Cameras and microphones work there without HTTPS because browsers treat
`localhost` as secure.

Not for any real deployment: the passwords and keys are throwaway, anyone who can open the page
can sign in as any demo user, and every port listens on 127.0.0.1 only.

## Pull and run

Needs only Docker with Compose v2: no git, no checkout, no build. `demo/release/compose.yaml`
names images that `demo-release.yml` built from one commit, tested together with the smoke test
and published (tag `demo-2d552eb4dc94`). In an empty directory:

```sh
mkdir ulw-demo && cd ulw-demo
curl -fsSLO https://raw.githubusercontent.com/Jerome-Joseph-1/ULW-Video-Streaming-Platform/demo/release/demo/release/compose.yaml
curl -fsSLO https://raw.githubusercontent.com/Jerome-Joseph-1/ULW-Video-Streaming-Platform/demo/release/demo/release/seccomp.json
docker compose pull
docker compose up -d
```

Then open http://localhost:8080. The first start takes a minute or two; `docker compose ps`
shows `web` as `healthy` once everything is ready. Keep `seccomp.json` beside `compose.yaml` and
run compose in that directory: it is the transcoder's sandbox profile.

```sh
docker compose down      # stop; videos and messages are kept
docker compose down -v   # stop and forget everything (videos, messages, keys)
```

On a Linux host with Ubuntu 24.04's AppArmor, the transcoder's sandbox also needs
`sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0` (Docker Desktop needs nothing).

## Start and stop

Needs only Docker (Docker Desktop on macOS or Windows, or Docker Engine on Linux) with Compose v2.

```sh
demo/up.sh          # pulls the images, starts everything, waits until ready, prints the URL
demo/down.sh        # stops; videos and messages are kept for the next up.sh
demo/down.sh --wipe # stops and forgets everything
```

The first `up.sh` downloads about 2.5 GB (the live recorder, LiveKit egress, is 1.5 GB of
it) and takes 3 to 10 minutes on a home connection. Later starts take about a minute. Run it
the evening before a demo.

| Setting | Default | |
|---|---|---|
| `DEMO_PORT` | `8080` | The page |
| `DEMO_S3_PORT` | `9900` | The store; the browser fetches video segments from it directly |
| `DEMO_RTC_TCP_PORT`, `DEMO_RTC_UDP_PORT` | `7881`, `7882` | Call and live media |
| `ULW_TAG` | `main` | Which published build of our images: `main`, or a commit's 40-character SHA, to pin a build that was checked (`DEMO_IMAGE_TAG` is the same setting) |
| `DEMO_BUILD=1` | off | Build our images from this checkout instead of pulling them (20 to 40 minutes) |
| `DEMO_LIVE=0` | on | Leave out the live recorder (saves the 1.5 GB download; going live then does not work) |

Our four images (`ghcr.io/jerome-joseph-1/ulw-video-gateway`, `-video-worker`, `-chat`,
`-live-packager`) are public and rebuilt from `main` on every merge (`publish-images.yml`), so
the demo shows whatever `main` is. They are the only images not pinned by digest. Every other
image is pinned by digest in `compose.yaml`. The vendored browser libraries are pinned too:
`web/vendor/` (hls.js 1.7.3 and livekit-client 2.22.3, the versions `tests/e2e` and `tests/call`
lock), loaded with SRI hashes.

### Resources

| | Needs |
|---|---|
| CPU | 4 cores. A transcode or a live recording uses 1 to 2 cores while it runs. |
| Memory | About 3 GB for the stack, plus about 1 GB more while a live stream is recorded. Give Docker Desktop 6 GB or more. Each browser window with a call or a camera uses another 300 to 500 MB. |
| Disk | About 8 GB for the images once unpacked (egress alone is 4.8 GB), plus what you upload. |

Our images are built for x86-64 only. On an Apple Silicon Mac, Docker Desktop runs them under
emulation (Rosetta): everything works, but a transcode takes a few times longer. Keep demo clips
short (under a minute). In Docker Desktop, under Settings > General, "Use Rosetta for x86_64/amd64
emulation" should be on.

## Demo day

The plan is the image archive a CI run built from `demo/integration` and tested twice (plan B
below is the primary plan): not every feature is on `main` yet. Everything runs on the
presenting laptop.

**Build for the demo:** `<sha12>` is the 12-digit commit and `<RUN>` the `e2e` run id; the
run's summary lists the archive `demo-images-<sha12>` (about 250 MB, kept 2 days from the run).
A run qualifies when it is a green `e2e` run of `demo/integration` dispatched with `demo` and
`demo_build` ticked, `sandbox` unticked: both smoke runs say `7 passed`.

### Prerequisites (the evening before)

- **Docker Desktop** (macOS or Windows) or Docker Engine with Compose v2 (Linux). In Docker
  Desktop, Settings > Resources: **4 CPUs, 6 GB memory** or more, and 10 GB of free disk image
  space.
- **Apple Silicon Mac:** Settings > General, "Use Rosetta for x86_64/amd64 emulation on Apple
  Silicon" on. Our images are amd64 and run emulated: everything works, transcodes are a few
  times slower, so keep demo clips under a minute.
- **git**, and the **GitHub CLI logged in** (`gh auth status` shows your account; if not,
  `gh auth login`). Without `gh`, download the archive from the run's page while signed in to
  GitHub (Summary > Artifacts > `demo-images-<sha12>`), then unzip it.
- **Chrome**, plus a second Chrome profile or another browser (Firefox, Edge) for carol.
- Ports 8080, 9900, 7881 (TCP) and 7882 (UDP) free on the laptop.

### Plan B (primary): the tested images, from the archive

Copy and paste, in a terminal, the evening before (the first start also downloads about 2 GB of
third-party images):

```sh
git clone https://github.com/Jerome-Joseph-1/ULW-Video-Streaming-Platform.git ulw && cd ulw
git checkout <sha12>
gh run download <RUN> -R Jerome-Joseph-1/ULW-Video-Streaming-Platform -n demo-images-<sha12>
demo/load.sh demo-images-<sha12>.tar.gz
ULW_TAG=local-<sha12> demo/up.sh
```

`gh run download` puts `demo-images-<sha12>.tar.gz` in the current directory (from the run's
page you get a zip: unzip it there). `load.sh` loads the four images the run built from that
commit (gateway, worker, chat, live packager), tagged `local-<sha12>`; `up.sh` uses them as
they are, never pulls them, and pulls only the third-party images (Postgres, MinIO, LiveKit,
Redis, egress, nginx, node), all pinned by digest. It checks the machine first (Docker running,
memory, free ports, disk), waits until everything is ready and prints "The demo is up". Keep the
checkout at `<sha12>`: the page and the browser's MLS client come from it and match the images.

Once it is up, run the [10-minute demo](#the-10-minute-demo) once as a rehearsal, then stop it
with `demo/down.sh`. The archive expires 2 days after the run; once loaded, the images stay in
Docker, so download and load it the evening before.

### Plan A (secondary): main, pinned

Only if the features are all on `main`: a `main` commit whose `publish-images` run succeeded and
whose demo smoke run was green, its full 40-digit SHA as `<SHA>`:

```sh
git checkout <SHA>
ULW_TAG=<SHA> demo/up.sh
```

### 30 minutes before

1. Docker Desktop running, with the resources above. Close heavy apps; disconnect any VPN.
2. In the checkout (still at `<sha12>`): `demo/down.sh --wipe` to forget the rehearsal's
   videos, messages and encrypted room, then `ULW_TAG=local-<sha12> demo/up.sh`. About a
   minute; it prints "The demo is up: http://localhost:8080".
3. Clear the rehearsal's browser data in every browser and profile you will use: on
   http://localhost:8080, the address bar's site icon > Site settings > Delete data (or close
   the Incognito window). This forgets each browser's MLS devices, which must match the wiped
   server.
4. Open the three windows (below), sign in, allow the camera and microphone in each.
5. Have a short video at hand (an `.mp4` or `.mov`, under a minute), or plan to use the page's
   **Make a 6 s test clip**.
6. Quick check: alice's **People** list shows bob and carol online (green).

### Browser profiles

Each user needs a browser of its own, because each keeps its own sign-in and MLS device:

| User | Window |
|---|---|
| **alice** | A normal Chrome window: http://localhost:8080, click **alice** |
| **bob** | A Chrome Incognito window: http://localhost:8080, click **bob** |
| **carol** | A second Chrome profile (profile icon > Add), or another browser: http://localhost:8080, click **carol** |

Put alice and bob side by side; carol is needed only for the group call. Always
`http://localhost:8080`, not a LAN address: browsers allow cameras only on localhost or HTTPS.

### After the demo

`demo/down.sh` (keeps the videos and messages), or `demo/down.sh --wipe` (forgets them).

If something misbehaves during the demo, move on to the next part; each tab stands on its own.
The [Troubleshooting](#troubleshooting) table covers the usual causes.

## What is running

`compose.yaml`, one network namespace shared by every service, so each reaches the others on
127.0.0.1 and the browser reaches the same addresses through the published ports:

| Service | What |
|---|---|
| `web` | nginx: the page (`web/`), and one origin for the gateway (`/api`), chat (`/rt`), LiveKit (`/rtc`, `/whip`) and the token issuer (`/auth`) |
| `gateway` | Uploads, videos, playlists, and the live stream service. It runs in the live packager's image, so that it can start one `live_packager` per stream itself (the `process` runtime) |
| `worker` | Transcodes to HLS with ffmpeg, in its sandbox |
| `chat` | Rooms, messages, presence, call tickets and the ring |
| `livekit`, `redis`, `egress` | The SFU for calls and live publishing, and the recorder that relays a live stream to its packager |
| `postgres`, `minio` | The database and the store (bucket `ulw-demo`, created at start) |
| `auth` | Stands in for an identity provider: signs tokens for alice, bob and carol with a key made on first start, and writes the key set the gateway and chat verify with |
| `migrate`, `seed` | One-shot: the schema, then the demo's rooms and member lists (`db/seed.sql`) |

## The 10-minute demo

Set up as in [30 minutes before](#30-minutes-before): alice in a normal Chrome window, bob in
an Incognito window side by side, carol in another profile or browser, each signed in with the
camera and microphone allowed.

1. **Upload and playback (2 min)**, alice, tab **Upload & Watch**.
   - Click **Choose file** and pick your clip, or click **Make a 6 s test clip**.
   - Click **Upload**. The bar fills chunk by chunk. To show that uploads resume, click
     **Pause** halfway, then **Resume**: it asks the gateway where it stands and continues.
   - Under **My videos** the clip goes `uploading`, `processing`, `ready`. Click **Play**: HLS
     plays, with its renditions listed under the player.
   - Point out: segments come straight from the store on signed URLs; only alice can see this
     video.
2. **Chat and presence (1 min)**, tab **Chat** in alice and bob.
   - The rooms are already there: each page opened a direct chat with every other user, and
     alice's page created the **team** group (alice, bob, carol) and the encrypted room (alice,
     bob), all through chat itself.
   - In alice, **People** shows bob online (green). Click the **bob** room in alice and the
     **alice** room in bob. Type in alice, press Enter: it appears in bob at once. Reply from bob.
   - Reload bob: the history is back, and bob goes offline and online for alice after the
     10 s grace.
3. **MLS encrypted room with Approve (2 min)**, still **Chat**.
   - In alice open **encrypted (alice + bob)** (the lock), then in bob. Alice's browser starts
     the MLS group. Bob's device asks to join, and a card appears at the top of alice's Chat
     tab: "bob asks to add the device bob/... Its fingerprint: ...". Compare it with the
     fingerprint bob's banner shows for its own device ("This device: ..."), then click
     **Approve**. Bob's device joins; both banners list the two devices and their fingerprints.
   - Send a message from bob; alice reads it. Tick **show what the server stores**: under each
     message is the body chat stored, an MLS ciphertext. The chat server never had a key.
   - The banner names the encryption: MLS (RFC 9420) ciphersuite 1
     (MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519), OpenMLS compiled to WebAssembly.
4. **1:1 call with ringing (1.5 min)**, tab **Calls** in alice and bob.
   - In alice click **Call** next to bob. Bob's window rings, whatever tab it is on: "alice is
     calling", with a ring tone, **Answer** and **Decline**. Click **Answer**: both see each
     other.
   - Try **Mute** and **Camera off**, then **Hang up** in alice; bob's call ends too.
   - Optional: call again and **Decline** in bob: alice stops calling.
5. **Group call of three with ringing (1.5 min)**, tab **Calls** in all three windows.
   - In alice click **Join group call** next to **team**. Bob and carol ring ("alice is
     calling"); click **Answer** in each: a grid of three.
   - **Hang up** in carol: the call goes on for alice and bob. Then **End for everyone** in
     alice ends it for all.
6. **Live: go live, watch, end, the recording (2 min)**, tab **Live** in alice and bob.
   - In alice click **Go live**. The camera preview starts; the status turns `LIVE` in a few
     seconds.
   - In bob, the stream shows under **Watch** (alice's stream, `live`). Click **Watch**: it
     plays a few seconds behind, with the delay shown, and a live chat beside it.
   - In alice click **End stream**. Bob's player ends. Alice's status says the recording is in
     **Upload & Watch**: open that tab; the recording goes `processing`, then `ready`. Click
     **Play**.

To start over with an empty catalog: `demo/down.sh --wipe`, clear the site's data in each
browser, and `ULW_TAG=local-<sha12> demo/up.sh` again.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `up.sh` says a port is in use (`bind: address already in use`) | Something else has 8080, 9900, 7881 or 7882. Pick others: `DEMO_PORT=8090 DEMO_S3_PORT=9910 demo/up.sh` (`DEMO_RTC_TCP_PORT`, `DEMO_RTC_UDP_PORT` likewise). Use the same values for `down.sh`. |
| `gh run download` says no artifact matches, or the run page lists none | The archive expired (2 days after its run) or the name is wrong: it is `demo-images-` and the 12-digit commit. Images already loaded stay in Docker: `docker image ls 'ghcr.io/jerome-joseph-1/*'` lists them, and `ULW_TAG=local-<sha12> demo/up.sh` still works. Otherwise use plan A. |
| `up.sh` says an image "is not here: load the archive first" | Run `demo/load.sh demo-images-<sha12>.tar.gz` first, and use the same `<sha12>` in `ULW_TAG=local-<sha12>`. |
| `up.sh` waits for a long time | `docker compose -f demo/compose.yaml ps -a` shows what is not up; `docker compose -f demo/compose.yaml logs <service>` says why. The first start's image downloads are the usual cause. |
| Videos stay `processing`, and `docker compose -f demo/compose.yaml logs worker` shows it restarting with an `unshare` error from `ulw_sandbox` | The worker's ffmpeg sandbox needs user namespaces. On Ubuntu 23.10 or later as the host: `sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`, then `docker compose -f demo/compose.yaml restart worker gateway`. Docker Desktop needs nothing. |
| A clip fails with "the file could not be decoded as video" | A WebM recorded by Firefox has no duration; use Chrome or Safari for the test clip, or upload a real file. |
| Calls connect but show no video, or live never turns `LIVE` | Usually a VPN: disconnect it (and quit its app if it filters even when disconnected), then reload the windows. The media ports (7881 TCP, 7882 UDP) must reach Docker; a firewall that intercepts loopback UDP blocks them too. Check `docker compose -f demo/compose.yaml logs livekit`. |
| No camera or microphone | Use `http://localhost:8080` (not a LAN address: browsers allow cameras only on localhost or HTTPS), and allow them in the address bar. On macOS also allow the browser under System Settings > Privacy & Security > Camera and Microphone. |
| Both windows show the same user | Each window keeps its own user; a window opened from another (Cmd+click) may copy it. Click **switch user**, or open `http://localhost:8080/?user=bob`. |
| Live: "busy" | The demo runs one live stream at a time. End the other (its owner's **End stream**), or wait about 2 minutes for an abandoned one to time out. |
| Live plays nothing for a long time | The recorder admits a stream only with CPU to spare; close other heavy apps. `docker compose -f demo/compose.yaml logs egress gateway` shows it. |
| The encrypted room says "waiting to be added" and stays so | The group was made by a browser that is gone (site data cleared, another browser). Click **reset encryption** in both windows (alice's first), or start clean: `demo/down.sh --wipe && demo/up.sh` and clear the site's data for localhost:8080. |
| Everything is stuck after the laptop slept | `demo/down.sh && demo/up.sh`. |

## How the demo maps onto the platform

- **Sign-in.** `auth` mints Ed25519 tokens exactly as `tools/devtoken` does, and the gateway and
  chat verify them against its key set through `ULW_DEV_JWKS_FILE`, allowed only with
  `ULW_DEV_MODE=1` (docs/integration/auth.md). The page sends the token as a bearer header,
  not the cookie, so two users can share one browser. A browser cannot put a header on a
  WebSocket, so the page passes it as `/rt?token=`, and the web proxy turns it into the header
  and strips it (nginx/default.conf; nothing logs it).
- **Rooms.** Where chat has the member-list commands (feat/chat-rooms-api), the page uses
  them (`web/js/rooms.js`): `open_direct` with each other user, alice's `create_group` for
  the team and the encrypted room, `rooms` and `members` to list them, and `member` frames to
  follow changes; `db/seed.sql` then does nothing. On a build without them, `db/seed.sql`
  lists the rooms in the database and `web/rooms.json` names them for the page.
- **Ringing.** Where chat rings (feat/call-ring), the page uses it: `call_ringing`,
  `call_answered` and the rest. Where it does not, the page rings through the direct chat
  itself with messages of its own, as docs/integration/calls.md says to.
- **Live streams.** No endpoint lists streams; the page announces its own in the team room and
  checks each with `GET /api/v1/live/{id}`.
- **E2EE.** `web/js/e2ee.js` runs MLS with the OpenMLS browser client (clients/web-mls/dist,
  served at `/mls/` from this checkout), every room body one MLSMessage, the group id the room
  id. Each browser profile keeps a device per user (identity `<user>/<id>`) in IndexedDB. The
  room's first member (alice) starts the group and adds whoever asks. Without the client in the
  checkout it uses `web/js/e2ee-standin.js`, labelled as not MLS.

## The smoke test

`demo/smoke/run.sh` starts the stack and drives every tab in headless Chrome with a fake
camera and microphone, as two or three users (`smoke.spec.mjs`). What a build does not serve
yet is checked to say so on the page and reported as skipped. In CI, run the `e2e` workflow by
hand with `demo` ticked (and `sandbox` unticked to skip the cluster job); `demo_build` builds
the images from the chosen branch instead of pulling `:main`.

```sh
demo/smoke/run.sh                  # up, then test
DEMO_SKIP_UP=1 demo/smoke/run.sh   # against a stack already up
```

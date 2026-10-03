# Integration guide

For Askedin's app and backend teams: everything needed to upload and play video through the ULW
service, and where the realtime features stand. Each page cites the source it was checked
against in HTML comments, for maintainers.

## What the service offers

- **Resumable uploads** of video files up to 50 GiB, in 8 MiB chunks, straight to object
  storage. An interrupted upload continues from the last durable byte.
- **Transcoding** to HLS (fMP4, H.264 and AAC) at 1080p, 720p and 360p, never above the source.
- **Playback** through rewritten HLS playlists; the segment bytes come from R2 over presigned
  URLs and never pass through the service.
- **Identity** from Askedin: the same JWT the apps already hold, verified against Askedin's
  JWKS. There are no ULW accounts.
- **Realtime** (chat, calls, live streams, end-to-end encryption): in progress; see the drafts.

## Pages

| Page | Covers | Status |
|---|---|---|
| [auth.md](auth.md) | Accepted tokens, the user id, rejections | Stable |
| [uploads.md](uploads.md) | Upload endpoints, resume, limits, errors, a curl walkthrough | Stable |
| [videos-and-playback.md](videos-and-playback.md) | Video states, playlists, presigned segments, CORS, players | Stable |
| [operations-contract.md](operations-contract.md) | What the platform provides; health, readiness, metrics | Stable |
| [versioning.md](versioning.md) | Compatibility and how changes are announced | Stable (policy is a proposal) |
| [changelog.md](changelog.md) | Changes to the Stable pages a client may have to act on | Stable |
| [chat.md](chat.md) | WebSocket endpoint, envelope, resume, history and member lists | Draft until phase 2 |
| [calls.md](calls.md) | 1:1 calls; group calls are planned, not available | Draft until M26 |
| [live.md](live.md) | Live streams | Draft until M33 |
| [e2ee.md](e2ee.md) | End-to-end encrypted chat | Draft until M22 |

"Stable" means the `/api/v1` contract described there is kept under the rules in
[versioning.md](versioning.md). "Draft" pages change without notice until the milestone named
at their top merges.

## Base URLs and environments

The ops team fills these in.

| | Stage | Prod |
|---|---|---|
| App origin (`<APP_ORIGIN>`) | `https://<STAGE_APP_HOST>` | `https://<PROD_APP_HOST>` |
| Video API (`$GW`) | `https://stage.askedin.com`, unconfirmed (paths `/api/v1/uploads`, `/api/v1/videos`, `/api/v1/live`) | `https://askedin.com`, `https://www.askedin.com` |
| Segment host (R2) | `https://<R2_ACCOUNT_ID>.r2.cloudflarestorage.com` | same form, prod bucket |
| Chat (`wss://<CHAT_HOST>/rt`) | not deployed yet | not deployed yet |
| Token cookie | `auth_token_stage` | `auth_token` |
| `JWT_ISSUER` | `https://auth-stage.askedin.com/auth`, unconfirmed ([auth.md](auth.md#askedin)) | `https://auth.askedin.com` |
| `JWKS_URL` | `https://auth-stage.askedin.com/.well-known/jwks.json` | `https://auth.askedin.com/.well-known/jwks.json` |
| `JWT_AUDIENCE` | `askedin-platform` | `askedin-platform` |

The video API is routed on Askedin's shared Envoy Gateway by path prefix on each environment's
app hosts above, so it answers on the app origin: a web page can call `/api/v1/...`
same-origin with its cookie.

## Quickstart: upload a file and play it

With `GW` and `TOKEN` set, `curl`, `jq` and bash:

```sh
F=clip.mp4; SIZE=$(stat -c %s "$F"); AUTH="Authorization: Bearer $TOKEN"
R=$(curl -sS -X POST "$GW/api/v1/uploads" -H "$AUTH" -d "{\"filename\":\"$F\",\"size_bytes\":$SIZE,\"content_type\":\"video/mp4\"}")
U=$(jq -r .upload_id <<<"$R"); V=$(jq -r .video_id <<<"$R"); CHUNK=$(jq -r .chunk_size <<<"$R"); OFF=0
while [ "$OFF" -lt "$SIZE" ]; do
  OFF=$(tail -c +$((OFF + 1)) "$F" | head -c "$CHUNK" | curl -sS -D - -o /dev/null -X PATCH "$GW/api/v1/uploads/$U" \
    -H "$AUTH" -H "Upload-Offset: $OFF" --data-binary @- | tr -d '\r' | awk -F': ' 'tolower($1)=="upload-offset"{print $2}')
done
curl -sS -X POST "$GW/api/v1/uploads/$U/commit" -H "$AUTH"
until curl -sS "$GW/api/v1/videos/$V" -H "$AUTH" | grep -q '"state":"ready"'; do sleep 5; done
curl -sS "$GW/api/v1/videos/$V/master.m3u8" -H "$AUTH"   # hand this URL to hls.js or AVPlayer
```

Each `PATCH` continues from the offset the server returned. This sketch stops on the first
error; a real client resumes from `HEAD` and stops polling on `failed`
([uploads.md](uploads.md#resuming)). Then play `$GW/api/v1/videos/$V/master.m3u8` as in
[videos-and-playback.md](videos-and-playback.md#playing).

## Conventions

- Ids are UUIDv7 in lowercase canonical form (`01a0ece4-69d0-781f-822e-f9f2e975cd5f`). Other
  spellings are `404`, not normalised.
- Error responses have an empty body. The status code, `Upload-Offset`, `Retry-After`,
  `Allow` and `WWW-Authenticate` headers carry everything. A `429` or `503` always carries
  `Retry-After`; wait that long before retrying.
- Every response has `X-Request-Id`. Quote it in bug reports.
- Only the owner can see a video or an upload. Anything else answers `404`.
- HTTP/1.1 with `Content-Length`; chunked request bodies are refused.

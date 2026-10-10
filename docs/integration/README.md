# Integration guide

For the teams building apps and backends on a ULW deployment: everything needed to upload and
play video through the service, and where the realtime features stand. Each page cites the source it was checked
against in HTML comments, for maintainers.

## What the service offers

- **Resumable uploads** of video files up to 50 GiB, in 8 MiB chunks, straight to object
  storage. An interrupted upload continues from the last durable byte.
- **Transcoding** to HLS (fMP4, H.264 and AAC) at 1080p, 720p and 360p, never above the source.
- **Playback** through rewritten HLS playlists; the segment bytes come from the object store
  (R2 or any S3-compatible store) over presigned URLs and never pass through the service.
- **Identity** from the operator's identity provider: the same JWT the apps already hold,
  verified against the provider's JWKS. There are no ULW accounts.
- **Realtime**: chat (direct and group chats, history, presence, the operator's service API),
  1:1 and group calls, and live streams are Stable; end-to-end encryption is a draft.

## Pages

| Page | Covers | Status |
|---|---|---|
| [auth.md](auth.md) | Accepted tokens, the user id, rejections | Stable |
| [uploads.md](uploads.md) | Upload endpoints, resume, limits, errors, a curl walkthrough | Stable |
| [videos-and-playback.md](videos-and-playback.md) | Video states, playlists, presigned segments, CORS, players | Stable |
| [operator-contract.md](operator-contract.md) | What the operator provides; settings, health, readiness, metrics | Stable |
| [versioning.md](versioning.md) | Compatibility and how changes are announced | Stable (policy is a proposal) |
| [changelog.md](changelog.md) | Changes to the Stable pages a client may have to act on | Stable |
| [chat.md](chat.md) | WebSocket endpoint, envelope, resume, history, member lists, presence and the service API | Stable |
| [calls.md](calls.md) | 1:1 and group calls | Stable |
| [live.md](live.md) | Live streams: starting, publishing over WHIP, watching, and the recording each becomes | Stable |
| [e2ee.md](e2ee.md) | End-to-end encrypted chat | Draft until M22 |

"Stable" means the contract described there (the `/api/v1` endpoints, and chat's WebSocket and
service API) is kept under the rules in [versioning.md](versioning.md). "Draft" pages change without notice until the milestone named
at their top merges.

## Base URLs and environments

Each deployment's operator fills these in for its apps; deploy/kubernetes/RUNBOOK.md has where
each value is set (most of them in the environment's `config.env`).

| | Value |
|---|---|
| App origin (`<APP_ORIGIN>`) | The web app's origin, listed in `ALLOWED_ORIGINS`, e.g. `https://video.example.com` |
| Video API (`$GW`) | The public host the routes answer (`PUBLIC_HOSTNAME`), paths `/api/v1/uploads`, `/api/v1/videos`, `/api/v1/live` |
| Segment host | The object store the presigned URLs name: `https://<R2_ACCOUNT_ID>.r2.cloudflarestorage.com`, or `S3_ENDPOINT` |
| Chat (`wss://<CHAT_HOST>/rt`) | The same public host, path `/rt` |
| Token cookie | `AUTH_COOKIE` (default `auth_token`) |
| `JWT_ISSUER`, `JWKS_URL`, `JWT_AUDIENCE` | The identity provider's ([auth.md](auth.md#configuring-an-identity-provider)) |

The Kubernetes deployment routes the video API on the operator's Gateway by path prefix on the
app's own host, so it answers on the app origin: a web page can call `/api/v1/...`
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

# Videos and playback

A video is created by an upload ([uploads.md](uploads.md)) or by the end of a live stream
([live.md](live.md#when-a-stream-ends)), transcoded to HLS by the worker, and played through the
gateway. The gateway authorizes each playlist request and rewrites the
playlist; the segment bytes come straight from the object store over presigned URLs and never
pass through the gateway (ADR-0002, ADR-0024).

Only the video's owner (the user whose token created the upload, or the broadcaster of the
stream it recorded) can see or play it in this version. For anyone else every endpoint below
answers `404`, exactly as for a video that does not exist.

## Lifecycle

<!-- core/include/core/models/video.hpp, infra/postgres/src/upload_catalog.cpp, apps/gateway/src/connection.cpp (state_name, on_committed) -->

| `state` | Entered when | Playable |
|---|---|---|
| `init` | `POST /api/v1/uploads` | no |
| `uploading` | The first `PATCH` to the upload completes, even one that made nothing durable yet | no |
| `processing` | `POST /api/v1/uploads/{id}/commit` succeeds, or a live stream's recording is stored; a transcode job is queued | no |
| `ready` | The worker has published the HLS renditions; `duration_ms` is set | yes |
| `failed` | Transcoding failed for good (the worker retries a job before giving up) | never |

`ready` and `failed` are final. A cancelled upload leaves its video in `init` or `uploading`
for good.

There is no push notification in this version. **Poll `GET /api/v1/videos/{id}`** after the
commit until `state` is `ready` or `failed`. A 5 s interval is reasonable; how long it takes
grows with the video's length. Stop polling on `failed`.

## Endpoints

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/connection.cpp (on_video, start_playlist, on_playlist), apps/gateway/src/playback.cpp -->

All need a token ([auth.md](auth.md)) and take no body. `{id}` is the `video_id` from the create
response: a lowercase canonical UUID. Anything else (uppercase included) answers `404`.

| Endpoint | Success | Errors |
|---|---|---|
| `GET /api/v1/videos/{id}` | `200`, `application/json`, the video object below | `401`, `404`, `503`, `500` |
| `GET /api/v1/videos/{id}/master.m3u8` | `200`, `application/vnd.apple.mpegurl`, the master playlist | `401`, `404`, `409`, `503`, `500` |
| `GET /api/v1/videos/{id}/{rendition}/index.m3u8` | `200`, `application/vnd.apple.mpegurl`, a media playlist | `401`, `404`, `409`, `503`, `500` |

The video object:

```json
{"id":"0199950c-...","title":"clip.mp4","state":"ready","version":2,"duration_ms":6000}
```

| Field | Type | Meaning |
|---|---|---|
| `id` | string | The video id |
| `title` | string | The `filename` given at create |
| `state` | string | One of the states above |
| `version` | integer | Rises by one on every state change. Use it to tell two answers apart, not as a count of anything. |
| `duration_ms` | integer or `null` | Set once `ready` |
| `error_reason` | string, only when `failed` | Why, in a short English phrase meant for the video's owner, such as `the file could not be decoded as video`, `transcoding exceeded its time budget` or `upload expired`. Show it or log it; do not parse it, as the wording may change. Absent in every other state. |

A failed video:

```json
{"id":"0199950c-...","title":"clip.mp4","state":"failed","version":3,"duration_ms":null,"error_reason":"the file could not be decoded as video"}
```

### Playlists

- The master lists one variant per rendition. Renditions are named `<height>p`: `1080p`,
  `720p` and `360p`, each only when the source is at least that tall; a source shorter than 360
  lines gets one rendition at its own height. Each variant URI is rewritten to the gateway path
  `/api/v1/videos/{id}/{rendition}/index.m3u8`, so it is fetched from the gateway with the same
  credentials as the master.
- A media playlist is fMP4 HLS. Every URI in it, the `#EXT-X-MAP` init segment included, is an
  absolute presigned `https://` URL on the object store (R2 in production).
- Presigned URLs are valid for `max(2 x duration, 1 hour)`, capped at 7 days. A viewer paused
  beyond that gets `403` from the store on the next segment; reloading the media playlist gives
  fresh URLs.
- Playlists are answered with `Cache-Control: private, max-age=60`. They hold URLs signed for
  one viewer: never put them in a shared cache.
- Each master fetch is recorded as one view.

What the local run of the [upload walkthrough](uploads.md#walkthrough) returned for its 720p
clip (store URLs shortened; the local store is MinIO, production is R2 over `https://`):

```
GET /api/v1/videos/01a0ece4-69d0-781f-822e-f9f2e975cd5f/master.m3u8

HTTP/1.1 200 OK
Content-Type: application/vnd.apple.mpegurl
Cache-Control: private, max-age=60

#EXTM3U
#EXT-X-VERSION:7
#EXT-X-STREAM-INF:BANDWIDTH=3220800,RESOLUTION=1280x720,CODECS="avc1.4d4028,mp4a.40.2"
/api/v1/videos/01a0ece4-69d0-781f-822e-f9f2e975cd5f/720p/index.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=1020800,RESOLUTION=640x360,CODECS="avc1.4d4028,mp4a.40.2"
/api/v1/videos/01a0ece4-69d0-781f-822e-f9f2e975cd5f/360p/index.m3u8
```

```
GET /api/v1/videos/01a0ece4-69d0-781f-822e-f9f2e975cd5f/720p/index.m3u8

HTTP/1.1 200 OK
Content-Type: application/vnd.apple.mpegurl
Cache-Control: private, max-age=60

#EXTM3U
#EXT-X-VERSION:7
#EXT-X-TARGETDURATION:4
#EXT-X-MEDIA-SEQUENCE:0
#EXT-X-PLAYLIST-TYPE:VOD
#EXT-X-INDEPENDENT-SEGMENTS
#EXT-X-MAP:URI="http://127.0.0.1:19471/ulw-integ/videos/01a0.../hls/720p/init_0.mp4?X-Amz-Algorithm=AWS4-HMAC-SHA256&...&X-Amz-Expires=3600&..."
#EXTINF:4.000000,
http://127.0.0.1:19471/ulw-integ/videos/01a0.../hls/720p/seg_00000.m4s?X-Amz-Algorithm=AWS4-HMAC-SHA256&...
#EXTINF:4.000000,
http://127.0.0.1:19471/ulw-integ/videos/01a0.../hls/720p/seg_00001.m4s?...
#EXTINF:4.000000,
http://127.0.0.1:19471/ulw-integ/videos/01a0.../hls/720p/seg_00002.m4s?...
#EXT-X-ENDLIST
```

The init segment and each media segment then came straight from the store with `200` and no
credentials; `GET .../1080p/index.m3u8` for this 720p source answered `404`, and the master
before the transcode finished answered `409`. The 12 s clip got the 1 h URL lifetime floor
(`X-Amz-Expires=3600`).

Playlist errors:

| Status | Meaning | Client action |
|---|---|---|
| `401` | No valid token | Refresh the token, retry once |
| `404` | Not your video, no such video, bad id, or a rendition the master does not list | Stop |
| `409` | The video is not `ready` (still processing, or `failed`) | Poll `GET /api/v1/videos/{id}`; fetch again once `ready` |
| `503` | The store is throttling or unreachable; `Retry-After: 5` | Retry after the delay |
| `500` | The stored playlist is broken or a URL could not be signed | Report with `X-Request-Id`; do not retry in a loop |

All error bodies are empty.

## CORS

<!-- docs/adr/0028-segments-fetched-cross-origin-without-credentials.md -->

The web app loads playlists same-origin (the operator's route sends `/api/v1/videos` on the app
origin to the gateway), so the auth cookie goes with them. Segments come from the store's host,
cross-origin, **without credentials**: the signature in the URL is the authorization. The
production bucket needs this CORS rule, one origin per environment, no `AllowCredentials`:

```json
{"CORSRules": [{
  "AllowedOrigins": ["https://<APP_ORIGIN>"],
  "AllowedMethods": ["GET", "HEAD"],
  "AllowedHeaders": ["Range", "If-None-Match"],
  "ExposeHeaders": ["Content-Range", "Content-Length", "ETag"],
  "MaxAgeSeconds": 3600
}]}
```

Without it every playlist request succeeds and playback fails at the first segment, in browsers
only. Native players are not subject to CORS.

## Playing

### hls.js (browsers without native HLS)

Keep the cookie on playlist requests and off segment requests. hls.js sends neither by default;
turn credentials on only for the gateway's own URLs:

```js
import Hls from "hls.js";

const src = `/api/v1/videos/${videoId}/master.m3u8`;
const video = document.querySelector("video");

if (Hls.isSupported()) {
  const hls = new Hls({
    xhrSetup(xhr, url) {
      // Same-origin gateway playlists: send the auth cookie. Presigned segment URLs: never.
      xhr.withCredentials = new URL(url, location.href).origin === location.origin;
    },
  });
  hls.on(Hls.Events.ERROR, (_, data) => {
    if (data.fatal && data.response?.code === 409) {
      // Not ready yet: poll GET /api/v1/videos/{id}, then hls.loadSource(src) again.
    }
  });
  hls.loadSource(src);
  hls.attachMedia(video);
} else if (video.canPlayType("application/vnd.apple.mpegurl")) {
  video.src = src;   // Safari and iOS: native HLS, cookie sent same-origin by the browser
}
```

A same-origin request carries the cookie even without `withCredentials`; the setting above
matters when the app is served from a different origin than the gateway, which then also needs
CORS on the gateway route, `ULW_ALLOWED_ORIGINS` listing the app, and `ULW_ALLOW_SAME_SITE=1` if
the app is on a sibling subdomain. The CORS layer must list the app's origin explicitly: never
echo `Origin` with credentials allowed ([auth.md](auth.md#cookies-and-other-sites)). For a bearer token instead of a cookie, set the header for gateway
URLs only:

```js
xhrSetup(xhr, url) {
  if (new URL(url, location.href).origin === GATEWAY_ORIGIN) {
    xhr.setRequestHeader("Authorization", `Bearer ${token}`);
  }
}
```

Never send the `Authorization` header to the store's host: the request would fail CORS
preflight and leak the token to another host.

### Native HLS (iOS, Android, Safari)

- Safari and `WKWebView` on the app origin: set the `<video>` `src` to the master URL; the cookie
  goes with the playlist requests.
- `AVPlayer`: put the token in a cookie in `HTTPCookieStorage` scoped to the gateway host.
  Headers set with `AVURLAssetHTTPHeaderFieldsKey` go with every request of the asset, segments
  included, and a presigned URL that also carries an `Authorization` header may be refused by
  the store.
- ExoPlayer / Media3: a `DefaultHttpDataSource.Factory` whose request properties add the bearer
  header only when the URI's host is the gateway host (a custom `DataSource.Factory` wrapping
  it), so segment requests go to the store unauthenticated.

Tokens expire. A player that runs longer than the token lives gets `401` on its next playlist
reload; refresh the token and reload.

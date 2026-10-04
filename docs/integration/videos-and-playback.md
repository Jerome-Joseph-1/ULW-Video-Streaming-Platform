# Videos and playback

A video is created by an upload ([uploads.md](uploads.md)) or by the end of a live stream
([live.md](live.md#when-a-stream-ends)), transcoded to HLS by the worker, and played through the
gateway. The gateway authorizes each playlist request and rewrites the
playlist; the segment bytes come straight from the object store over presigned URLs and never
pass through the gateway (ADR-0002, ADR-0024).

A video's owner (the user whose token created the upload, or the broadcaster of the stream it
recorded) decides who else may see and play it ([Who can see a video](#who-can-see-a-video)).
For anyone who may not, every endpoint below answers `404`, exactly as for a video that does not
exist.

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
| `error_reason` | string, only when `failed`, owner only | Why, in a short English phrase meant for the video's owner, such as `the file could not be decoded as video`, `transcoding exceeded its time budget` or `upload expired`. Show it or log it; do not parse it, as the wording may change. Absent in every other state, and for anyone but the owner. |
| `visibility` | string, owner only | Who else may see it: `private`, `unlisted` or `room:<room id>` ([Who can see a video](#who-can-see-a-video)). Absent for anyone but the owner. |

A failed video, as its owner sees it:

```json
{"id":"0199950c-...","title":"clip.mp4","state":"failed","version":3,"duration_ms":null,"error_reason":"the file could not be decoded as video","visibility":"private"}
```

Anyone else who may see it gets the same object without `error_reason` and `visibility`.

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
| `404` | A video you may not see, no such video, bad id, or a rendition the master does not list | Stop |
| `409` | The video is not `ready` (still processing, or `failed`) | Poll `GET /api/v1/videos/{id}`; fetch again once `ready` |
| `503` | The store is throttling or unreachable; `Retry-After: 5` | Retry after the delay |
| `500` | The stored playlist is broken or a URL could not be signed | Report with `X-Request-Id`; do not retry in a loop |

All error bodies are empty.

## Who can see a video

<!-- core/src/video_access.cpp (access_of), core/src/visibility.cpp, infra/postgres/src/upload_catalog.cpp (kFindVideoFor, kSetVisibility, kGrantAccess), apps/gateway/src/connection.cpp (on_video, start_update, start_service_route), apps/gateway/src/video_access.cpp, migrations/0016_video_access.sql, docs/adr/0097-videos-shared-by-visibility-and-service-grants.md -->

Every video has a **visibility**, which its owner sets:

| Visibility | Who besides the owner may see and play it |
|---|---|
| `private` | Nobody. Every video starts so, a live stream's recording included. |
| `unlisted` | Any signed-in user who has the video id. |
| `room:<room id>` | The current members of that chat room, direct or group: whoever chat lists in it ([chat.md](chat.md#changing-member-lists)) at the moment of each request. Someone taken off the list loses the video at their next request; someone added gains it. |

On top of any visibility, the operator's backend may **grant** a video to single users
([Service API](#service-api-grants)); a grant lasts until it is revoked.

Whatever the visibility and the grants say, a video whose upload is still in progress (`init`,
`uploading`) is its owner's alone. Once committed (`processing`, `ready`, `failed`) it follows
the rules above.

The same check runs on every request for the video's metadata and for both playlists: the media
playlist, and so every presigned segment URL, is only built for someone who may see the video.
Nothing is cached, on any gateway. A segment URL already handed out stays valid until it expires
(see [Playlists](#playlists)); revoking access stops new playlists, not URLs already issued.

There is no listing of videos in this version: a client keeps the ids it was given (by the
upload, by a chat message, by the operator's product).

### Setting the visibility

| Endpoint | Success | Errors |
|---|---|---|
| `PATCH /api/v1/videos/{id}` | `200`, `application/json`, the video object as its owner sees it | `400`, `401`, `403`, `404`, `413`, `503`, `500` |

The owner only. The body is JSON, `Content-Type: application/json`, at most 4 KiB:

```json
{"visibility":"room:0192f3c4-7a1b-7c2d-8e3f-0123456789ab"}
```

`visibility` is `private`, `unlisted` or `room:<room id>`, the room id a lowercase canonical UUID.
A room must be one the owner is a member of at that moment (a direct chat they are one of the
two of, or a group they are listed in); the owner leaving the room later takes nothing from its
members. A cookie request needs an allowed `Origin`, as an upload's create does
([auth.md](auth.md#cookies-and-other-sites)).

Refusals carry a JSON body, `{"error":"<code>"}`:

| Status | `error` | Meaning |
|---|---|---|
| `400` | `bad_visibility` | The body is not an object with one of the three forms; an empty body is `400` with no body |
| `403` | `forbidden` | You may see the video but are not its owner |
| `403` | `not_member` | The room is not one you are a member of |
| `404` | `not_found` | No such video, a bad id, or a video you may not see: the three are indistinguishable |

`401`, `413` and `503` (`Retry-After: 5`) come with an empty body, as elsewhere.

### Service API: grants

For the operator's backend only: a token whose claim `ULW_SERVICE_CLAIM` (default `scope`) holds
`ULW_SERVICE_SCOPE` ([auth.md](auth.md#service-tokens)), typically one the backend obtains from
the identity provider with the client-credentials grant. Any other token is `403` `forbidden`
on all three, before anything about the video is looked at; with `ULW_SERVICE_SCOPE` unset, every
token is.

| Endpoint | Success | Errors |
|---|---|---|
| `POST /api/v1/service/videos/{id}/grants/{user}` | `204`: `{user}` may see the video. Granting again changes nothing. | `400`, `401`, `403`, `404`, `503`, `500` |
| `DELETE /api/v1/service/videos/{id}/grants/{user}` | `204`: the grant is gone. Revoking one not held changes nothing. | `400`, `401`, `403`, `404`, `503`, `500` |
| `GET /api/v1/service/videos/{id}/grants` | `200`, `application/json`, `Cache-Control: no-store`, a page of grants | `400`, `401`, `403`, `404`, `503`, `500` |

`{user}` is the user id as the user's token names it (its subject claim, [auth.md](auth.md#how-the-user-id-is-derived)),
percent-encoded or not (`auth0%7C123` and `auth0|123` are the same user). None takes a body.
A grant is not checked against anything: a user who never signed in simply holds it.

The listing takes `limit` (1 to 1000, default 100) and `after` (a user id from a previous
page's `next`, percent-encoded) in its query, and returns grants in bytewise order of user id:

```json
{"video_id":"0199950c-...","grants":[{"user_id":"auth0|123","granted_at":1759600000}],"next":"auth0|123"}
```

`granted_at` is in Unix seconds; `next` is `null` on the last page.

| Status | `error` | Meaning |
|---|---|---|
| `400` | `bad_user` | `{user}` does not decode to a user id |
| `400` | `bad_query` | `limit` or `after` malformed, or given twice |
| `403` | `forbidden` | Not a service token |
| `404` | `not_found` | No such video, or a bad id. The service may learn this; nobody else does. |

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

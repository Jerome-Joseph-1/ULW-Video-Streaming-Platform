# Videos and playback

A video is created by an upload ([uploads.md](uploads.md)) or by the end of a live stream
([live.md](live.md#when-a-stream-ends)), transcoded to HLS by the worker, and played through the
gateway. The gateway authorizes each playlist request and rewrites the
playlist; the segment bytes come straight from the object store over presigned URLs and never
pass through the gateway (ADR-0002, ADR-0024).

A video's owner (the user whose token created the upload, or the broadcaster of the stream it
recorded) decides who else may see and play it ([Who can see a video](#who-can-see-a-video)).
For anyone who may not, every endpoint below answers `404`, exactly as for a video that does not
exist. The owner may delete the video ([Deleting a video](#deleting-a-video)) and give it
captions ([Captions](#captions)), and the operator's backend may set its visibility, take it
down and list a user's videos ([Service API](#service-api-visibility-takedown-and-listing)).

## Lifecycle

<!-- core/include/core/models/video.hpp, infra/postgres/src/upload_catalog.cpp, apps/gateway/src/connection.cpp (state_name, on_committed) -->

| `state` | Entered when | Playable |
|---|---|---|
| `init` | `POST /api/v1/uploads` | no |
| `uploading` | The first `PATCH` to the upload completes, even one that made nothing durable yet | no |
| `processing` | `POST /api/v1/uploads/{id}/commit` succeeds, or a live stream's recording is stored; a transcode job is queued | no |
| `ready` | The worker has published the HLS renditions; `duration_ms` is set | yes |
| `failed` | Transcoding failed for good (the worker retries a job before giving up) | never |

`ready` and `failed` are final. A video in any state may be deleted, by its owner or by the
operator's backend, and cancelling its upload deletes it too; from then on it is gone, a `404` on
every endpoint to everyone, its owner included ([Deleting a video](#deleting-a-video)).

There is no push notification in this version. **Poll `GET /api/v1/videos/{id}`** after the
commit until `state` is `ready` or `failed`, and show `progress` while it is `processing`
([Progress](#progress)). A 5 s interval is reasonable; how long it takes grows with the video's
length. Stop polling on `ready` or `failed`.

### Progress

<!-- apps/gateway/src/video_access.cpp (video_json), infra/postgres/src/upload_catalog.cpp (kFindVideoFor, decode_progress), apps/worker/src/job_runner.cpp (KeeperProgress, upload), docs/adr/0101-transcode-progress-on-the-video-object.md -->

While a video is `processing`, its object carries how far along the transcode is (ADR-0101):

```json
{"id":"0199950c-...","title":"clip.mp4","state":"processing","version":2,"duration_ms":null,"progress":{"stage":"transcoding","percent":42}}
```

| `progress.stage` | Meaning | `progress.percent` |
|---|---|---|
| `queued` | Waiting for a worker: the first attempt not started yet, or a retry waiting its turn after a transient failure | `0` |
| `transcoding` | A worker is on it: encoding, then publishing the renditions | `0` to `99`, rising |

- `progress` is `null` in every other state. `100%` is `state: "ready"`; `percent` never reaches
  100 while the video cannot play yet.
- The worker reports about every 10 s, so `percent` moves in steps: encoding takes it to 90,
  publishing the renditions from 90 to 99.
- After a transient failure the job is retried: `stage` goes back to `queued` and `percent` to
  0, then rises again. A progress bar should follow the value, not assume it only grows.
- Everyone who may see the video sees its progress, and the operator's backend sees it in the
  [listing](#service-api-visibility-takedown-and-listing).

A minimal poller:

```js
async function waitUntilPlayable(id, token, onProgress) {
  for (;;) {
    const r = await fetch(`/api/v1/videos/${id}`, {headers: {Authorization: `Bearer ${token}`}});
    if (!r.ok) throw new Error(`status ${r.status}`);
    const v = await r.json();
    if (v.state === "ready") return onProgress(100), v;
    if (v.state === "failed") throw new Error(v.error_reason ?? "transcoding failed");
    onProgress(v.progress?.percent ?? 0, v.progress?.stage ?? "queued");
    await new Promise((done) => setTimeout(done, 5000));
  }
}
```

## Endpoints

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/connection.cpp (on_video, start_playlist, on_playlist, start_own_listing), apps/gateway/src/playback.cpp (build_playlist, build_caption_playlist), core/src/hls.cpp (add_subtitles, subtitle_playlist), apps/gateway/src/video_access.cpp (own_video_query, videos_json) -->

All need a token ([auth.md](auth.md)) and take no body. `{id}` is the `video_id` from the create
response: a lowercase canonical UUID. Anything else (uppercase included) answers `404`.

| Endpoint | Success | Errors |
|---|---|---|
| `GET /api/v1/videos` | `200`, `application/json`, `Cache-Control: no-store`, a page of your own videos ([Your videos](#your-videos)) | `400`, `401`, `503`, `500` |
| `GET /api/v1/videos/{id}` | `200`, `application/json`, the video object below | `401`, `404`, `503`, `500` |
| `GET /api/v1/videos/{id}/master.m3u8` | `200`, `application/vnd.apple.mpegurl`, the master playlist | `401`, `404`, `409`, `503`, `500` |
| `GET /api/v1/videos/{id}/{rendition}/index.m3u8` | `200`, `application/vnd.apple.mpegurl`, a media playlist | `401`, `404`, `409`, `503`, `500` |
| `GET /api/v1/videos/{id}/captions/{language}/index.m3u8` | `200`, `application/vnd.apple.mpegurl`, the subtitles playlist of the video's captions in `{language}` ([Captions](#captions)) | `401`, `404`, `409`, `503`, `500` |

The video object:

```json
{"id":"0199950c-...","title":"clip.mp4","state":"ready","version":2,"duration_ms":6000,"progress":null}
```

| Field | Type | Meaning |
|---|---|---|
| `id` | string | The video id |
| `title` | string | The `filename` given at create |
| `state` | string | One of the states above |
| `version` | integer | Rises by one on every state change. Use it to tell two answers apart, not as a count of anything. |
| `duration_ms` | integer or `null` | Set once `ready` |
| `progress` | object or `null` | While `processing`, `{"stage": "queued" or "transcoding", "percent": 0 to 99}`; `null` in every other state ([Progress](#progress)) |
| `error_reason` | string, only when `failed`, owner only | Why, in a short English phrase meant for the video's owner, such as `the file could not be decoded as video`, `transcoding exceeded its time budget` or `upload expired`. Show it or log it; do not parse it, as the wording may change. Absent in every other state, and for anyone but the owner. |
| `visibility` | string, owner only | Who else may see it: `private`, `unlisted`, `public` or `room:<room id>` ([Who can see a video](#who-can-see-a-video)). Absent for anyone but the owner. |

A failed video, as its owner sees it:

```json
{"id":"0199950c-...","title":"clip.mp4","state":"failed","version":3,"duration_ms":null,"progress":null,"error_reason":"the file could not be decoded as video","visibility":"private"}
```

Anyone else who may see it gets the same object without `error_reason` and `visibility`.

### Your videos

`GET /api/v1/videos` lists the caller's own uploads (and live recordings), every state included
and deleted ones left out, newest first: the page the [service API's
listing](#service-api-visibility-takedown-and-listing) gives for the token's own user. Videos
others shared with you are not yours and are not listed. Its query, both optional:

| Parameter | Meaning |
|---|---|
| `limit` | 1 to 200, default 50 |
| `after` | The `next` of the previous page, as given. Opaque: do not build one |

```json
{"owner":"auth0|123","videos":[{"id":"0199950c-...","title":"clip.mp4","state":"processing","version":2,"duration_ms":null,"progress":{"stage":"transcoding","percent":40},"visibility":"private","created_at":1759600000}],"next":null}
```

Each video is the object above as its owner sees it, with `created_at` in Unix seconds; `next`
is `null` on the last page. An `owner` parameter, or a `limit` or `after` that is malformed or
given twice, is `400` with `{"error":"bad_query"}`. One request shows the progress of every
upload in flight, instead of polling each video.

### Playlists

- The master lists one variant per rendition, and the video's captions, if it has any, as a
  subtitles rendition per language ([Captions](#captions)). Renditions are named `<height>p`: `1080p`,
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
| `404` | A video you may not see, no such video, bad id, or a rendition or a captions language the master does not list | Stop |
| `409` | The video is not `ready` (still processing, or `failed`) | Poll `GET /api/v1/videos/{id}`; fetch again once `ready` |
| `503` | The store is throttling or unreachable; `Retry-After: 5` | Retry after the delay |
| `500` | The stored playlist is broken or a URL could not be signed | Report with `X-Request-Id`; do not retry in a loop |

All error bodies are empty.

## Captions

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/connection.cpp (start_caption, on_caption_view, on_caption_stored, on_caption_removed, start_playlist), apps/gateway/src/playback.cpp (caption_key, build_caption_playlist), core/src/webvtt.cpp (check), core/src/caption_language.cpp, core/src/hls.cpp (add_subtitles, subtitle_playlist), apps/gateway/src/video_access.cpp (caption_error_json), infra/postgres/src/upload_catalog.cpp (kPutCaption, kRemoveCaption, kListCaptions), migrations/0030_video_captions.sql, docs/adr/0139-a-video-carries-captions-in-webvtt.md -->

A video may carry captions: one WebVTT file per language, which its owner (or the operator's
backend, [below](#service-api-captions)) uploads. The master playlist lists each language as a
subtitles rendition, and players show them in their captions menu. Nothing writes captions for
you: speech to text is not part of the platform, so bring the file from your editor, a
captioning service or your own transcription step. The file is also the transcript: fetch it
from the URL in its playlist to show or search the text.

| Endpoint | Success | Errors |
|---|---|---|
| `PUT /api/v1/videos/{id}/captions/{language}` | `204`: the file is stored and listed, replacing the one before | `400`, `401`, `403`, `404`, `413`, `429`, `503`, `500` |
| `DELETE /api/v1/videos/{id}/captions/{language}` | `204`: the language is no longer listed and its file is gone. Deleting one not there changes nothing. | `400`, `401`, `403`, `404`, `503`, `500` |

The owner only. `{language}` is a BCP 47 tag of the plain shape: 2 or 3 letters, then subtags
of 1 to 8 letters or digits after `-` (`en`, `pt-BR`, `zh-Hant`, `es-419`), at most 35
characters. Case does not matter, and the playlists give it lowercase: `en-US` and `en-us` are
the same file. The `PUT`'s body is the file itself, at most 512 KiB, sent as
`Content-Type: text/vtt`; a cookie request needs an allowed `Origin`, as the PATCH does
([auth.md](auth.md#cookies-and-other-sites)). Captions may be put in any state, before the
upload is committed too; their playlists answer `409` until the video is `ready`, as its own do.

```
PUT /api/v1/videos/0199950c-.../captions/en
Authorization: Bearer <token>
Content-Type: text/vtt
Content-Length: 78

WEBVTT

00:00.000 --> 00:02.500
Hello.

00:02.500 --> 00:04.000
Welcome back.
```

A file is taken only when every player will read it as written:

- UTF-8 (a byte order mark is allowed), without NUL.
- The first line is `WEBVTT`, alone or after a space or a tab with anything after it. Header
  lines up to the first blank line are allowed, `X-TIMESTAMP-MAP` among them.
- Blocks are separated by blank lines, and each is a cue, a `NOTE`, or a `STYLE` or `REGION`
  ahead of the first cue. A cue is an optional identifier line, then its timing,
  `<start> --> <end>`, then its text. Lines may end in CR LF, LF or CR.
- Times are `mm:ss.ttt` or `hh:mm:ss.ttt` (two or more digits of hours), minutes and seconds
  below 60, and a cue ends after it starts. Cue settings after the end time are passed through
  as they are.
- Cues come in the order they start (two may start together, and overlap).
- `-->` appears in timing lines only: not in a cue's text, an identifier, a note or the header.

Cue times are the video's own, from 0. Nothing else in the file is looked into: its markup
(`<v Alice>`, `<i>`) and settings are the player's.

Refusals carry a JSON body:

| Status | `error` | Meaning |
|---|---|---|
| `400` | `bad_captions` | The file breaks a rule above. `reason` says which (`encoding`, `header`, `timing`, `order` or `block`) and `line` where, 1-based: `{"error":"bad_captions","reason":"timing","line":12}` |
| `400` | `bad_language` | `{language}` is not a tag of the shape above |
| `403` | `forbidden` | You may see the video but are not its owner |
| `404` | `not_found` | No such video, a bad id, or a video you may not see: the three are indistinguishable |

An empty body is `400` and one over 512 KiB `413`, both with no body. A `PUT` counts as one of
your uploads in flight while its body is read: `429` once three are, `503` when the gateway is
full ([uploads.md](uploads.md)). `401` and `503` (`Retry-After: 5`) come with an empty body, as
elsewhere; a `503` is safe to retry, as is a `DELETE` that failed.

### In the playlists

Once a language is listed, the master names it ahead of the variants, all in one group that
every variant names:

```
#EXTM3U
#EXT-X-VERSION:7
#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="subs",NAME="en",LANGUAGE="en",DEFAULT=NO,AUTOSELECT=YES,URI="/api/v1/videos/0199950c-.../captions/en/index.m3u8"
#EXT-X-STREAM-INF:BANDWIDTH=3220800,RESOLUTION=1280x720,CODECS="avc1.4d4028,mp4a.40.2",SUBTITLES="subs"
/api/v1/videos/0199950c-.../720p/index.m3u8
```

Languages come in the order of their tags. `NAME` is the tag too: label the track in your own
UI if the player shows the tag. `DEFAULT=NO` leaves captions off until the viewer, or a player
following the system's language (`AUTOSELECT=YES`), turns them on.

The subtitles playlist is one segment, the file's presigned URL, as long as the video:

```
#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:6
#EXT-X-MEDIA-SEQUENCE:0
#EXT-X-PLAYLIST-TYPE:VOD
#EXTINF:5.250,
https://<store>/videos/0199950c-.../hls/captions/en.vtt?X-Amz-Signature=...
#EXT-X-ENDLIST
```

It is fetched from the gateway with the same credentials as the master and answered as the
media playlists are ([Playlists](#playlists)): the same access check, the same URL lifetime,
`Cache-Control: private, max-age=60`, and the same errors. A language the master does not list
is `404`. A file replaced is served from the next playlist fetch on; one deleted stops being
listed at once, though a URL handed out before stays valid until it expires. The file comes from
the store with the store's default content type, not `text/vtt`: hls.js and the native players
read it either way, and the bucket's CORS rule ([CORS](#cors)) covers it.

With hls.js the captions are text tracks of the `<video>`: `hls.subtitleTracks` lists them and
`hls.subtitleTrack = i` (or `-1`) turns one on or off. Safari, `AVPlayer` and ExoPlayer show them
in their own captions menu.

### Service API: captions

The operator's backend may put and delete any video's captions, with the token and the rules of
its other routes ([Service API](#service-api-grants)): anything else is `403` `forbidden` before
the video is looked at.

| Endpoint | Success | Errors |
|---|---|---|
| `PUT /api/v1/service/videos/{id}/captions/{language}` | `204`, as the owner's `PUT` | `400`, `401`, `403`, `404`, `413`, `429`, `503`, `500` |
| `DELETE /api/v1/service/videos/{id}/captions/{language}` | `204`, as the owner's `DELETE` | `400`, `401`, `403`, `404`, `503`, `500` |

Bodies, checks and refusals are the owner's. `404` `not_found` is a video that does not exist
(deleted ones included); nothing is stored for it.

## Who can see a video

<!-- core/src/video_access.cpp (access_of), core/src/visibility.cpp, infra/postgres/src/upload_catalog.cpp (kFindVideoFor, kSetVisibility, kGrantAccess), apps/gateway/src/connection.cpp (on_video, start_update, start_service_route, on_caption_view), apps/gateway/src/video_access.cpp, migrations/0016_video_access.sql, migrations/0024_public_videos.sql, docs/adr/0097-videos-shared-by-visibility-and-service-grants.md, docs/adr/0121-a-public-video-plays-without-a-token.md -->

Every video has a **visibility**, which its owner sets:

| Visibility | Who besides the owner may see and play it |
|---|---|
| `private` | Nobody. Every video starts so, a live stream's recording included. |
| `unlisted` | Any signed-in user who has the video id. |
| `public` | Anyone who has the video id, signed in or not: its metadata and both playlists are served without a token ([Playing without a token](#playing-without-a-token)). |
| `room:<room id>` | The current members of that chat room, direct or group: whoever chat lists in it ([chat.md](chat.md#changing-member-lists)) at the moment of each request, while it also lists the owner. Someone taken off the list loses the video at their next request; someone added gains it. When the owner leaves the room, nobody in it sees the video any more; listed again, the owner shares into it again. |

On top of any visibility, the operator's backend may **grant** a video to single users
([Service API](#service-api-grants)); a grant lasts until it is revoked.

Whatever the visibility and the grants say, a video whose upload is still in progress (`init`,
`uploading`) is its owner's alone. Once committed (`processing`, `ready`, `failed`) it follows
the rules above.

The same check runs on every request for the video's metadata and for every playlist, its
captions' included: the media playlist, and so every presigned segment URL, is only built for
someone who may see the video.
Nothing is cached, on any gateway. A segment URL already handed out stays valid until it expires
(see [Playlists](#playlists)); revoking access stops new playlists, not URLs already issued.

There is no listing of videos for users in this version: a client keeps the ids it was given (by
the upload, by a chat message, by the operator's product). The operator's backend lists a user's
videos ([Listing a user's videos](#listing-a-users-videos)) and may hand them to its own client.

### Playing without a token

A request for a video's metadata (`GET /api/v1/videos/{id}`) or any of its playlists, its
captions' included, that carries no token at all (no `Authorization` header, no auth cookie) is served when the video is `public`
and its upload is committed, exactly as for a signed-in viewer: the video object without its
visibility, and playlists whose segment URLs are presigned as usual. Embed a player on a page
anyone can open and point it at the master playlist.

For anything else (a video that is not public, one still uploading, one that does not exist) a
request without a token is `401` with `WWW-Authenticate: Bearer`, as before, so a player that
lost its session still knows to sign in, and nobody learns whether an id exists. A token that
fails is `401` whatever the video, never served as if no token was sent. Requests without a token
count against their address's limits, and are not counted as views. Only the video routes take
them: uploads, listings and every change still need a token, and a live stream is never public.

### Setting the visibility

| Endpoint | Success | Errors |
|---|---|---|
| `PATCH /api/v1/videos/{id}` | `200`, `application/json`, the video object as its owner sees it | `400`, `401`, `403`, `404`, `413`, `503`, `500` |

The owner only. The body is JSON, `Content-Type: application/json`, at most 4 KiB:

```json
{"visibility":"room:0192f3c4-7a1b-7c2d-8e3f-0123456789ab"}
```

`visibility` is `private`, `unlisted`, `public` or `room:<room id>`, the room id a lowercase
canonical UUID.
A room must be one the owner is a member of at that moment (a direct chat they are one of the
two of, or a group they are listed in). Leaving the room later stops the share for everyone in it,
until the owner is listed again or sets another visibility. A cookie request needs an allowed `Origin`, as an upload's create does
([auth.md](auth.md#cookies-and-other-sites)).

Refusals carry a JSON body, `{"error":"<code>"}`:

| Status | `error` | Meaning |
|---|---|---|
| `400` | `bad_visibility` | The body is not an object with one of the four forms; an empty body is `400` with no body |
| `403` | `forbidden` | You may see the video but are not its owner |
| `403` | `not_member` | The room is not one you are a member of |
| `404` | `not_found` | No such video, a bad id, or a video you may not see: the three are indistinguishable |

`401`, `413` and `503` (`Retry-After: 5`) come with an empty body, as elsewhere.

### Deleting a video

<!-- apps/gateway/src/connection.cpp (start_delete, on_deleted), infra/postgres/src/upload_catalog.cpp (kDeleteVideo), infra/postgres/src/upload_reaper.cpp (kDuePurges, kForgetPurged), apps/reaper/src/reaper.cpp (purge_videos), migrations/0017_video_purges.sql, docs/adr/0100-the-operators-backend-controls-vod.md, docs/adr/0106-deleting-a-video-mid-upload-aborts-the-upload.md -->

| Endpoint | Success | Errors |
|---|---|---|
| `DELETE /api/v1/videos/{id}` | `204`: the video is gone | `400`, `401`, `403`, `404`, `503`, `500` |

The owner only, and no body. A video in any state may be deleted; a transcode still waiting for
it is cancelled, and one already running is left to finish unseen. A video whose upload is still
in progress (`init` or `uploading`) has its upload aborted in the same step, as a cancel would:
a later `PATCH` or commit of that upload answers `409` (`410` once it would have expired, `404`
once the reaper has removed it), and never brings the video back
([uploads.md](uploads.md#cancel-delete-apiv1uploadsid)).
From the `204` on, the video is gone for everyone, its owner included, on every gateway: its
metadata and both playlists answer `404`, exactly as for a video that never existed, and so do
the PATCH, a repeat of the owner's delete aside, and the service API's routes for it. Its grants
are removed, and a room's members and the holders of grants lose it with everyone else. Its
stored objects (the source file and every HLS rendition) are removed by the upload reaper at its
next pass, within about 15 minutes (operator-contract.md); until then nothing can reach them but
a segment URL a playlist handed out before the delete, which stays valid until it expires
([Playlists](#playlists)), as after a revocation.

Deleting is idempotent: a repeat answers `204` again until the reaper has removed the video for
good, and `404` after, like any id that does not exist. Treat both as "deleted". There is no
undo through the API.

A cookie request needs an allowed `Origin`, as for the PATCH ([auth.md](auth.md#cookies-and-other-sites)).
Refusals carry a JSON body, `{"error":"<code>"}`:

| Status | `error` | Meaning |
|---|---|---|
| `403` | `forbidden` | You may see the video but are not its owner |
| `404` | `not_found` | No such video, a bad id, or a video you may not see: the three are indistinguishable |

A body is `400`; `401` and `503` (`Retry-After: 5`) come with an empty body, as elsewhere.

### Service API: grants

For the operator's backend only: a token whose claim `ULW_SERVICE_CLAIM` (default `scope`) holds
`ULW_SERVICE_SCOPE` and, when `ULW_SERVICE_CLIENT_ID` is set, that was issued to that client
([auth.md](auth.md#service-tokens)), typically one the backend obtains from the identity provider
with the client-credentials grant. It must come in the `Authorization: Bearer` header: the cookie
never carries a service token. Any other request is `403` `forbidden` on all three, before
anything about the video is looked at; with `ULW_SERVICE_SCOPE` unset, every one is.

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

### Service API: visibility, takedown and listing

<!-- apps/gateway/src/connection.cpp (start_service_route, on_videos, on_service_deleted, set_service_visibilities), apps/gateway/src/video_access.cpp (video_query, videos_json, bulk_visibility_from_body), infra/postgres/src/upload_catalog.cpp (kSetVisibility, kDeleteVideo, kListVideos), docs/adr/0100-the-operators-backend-controls-vod.md, docs/adr/0119-one-visibility-for-many-videos.md -->

The operator's backend controls any user's videos: what the policy is (who may see what, what
is taken down) is the product's; these routes are how it applies it. The same token as for
grants, in the `Authorization` header only; anything else is `403` `forbidden` before the video
or the query is looked at.

| Endpoint | Success | Errors |
|---|---|---|
| `PATCH /api/v1/service/videos/{id}` | `200`, `application/json`, the video object as its owner sees it | `400`, `401`, `403`, `404`, `413`, `503`, `500` |
| `PATCH /api/v1/service/videos` | `200`, `application/json`, the videos changed and those that were not | `400`, `401`, `403`, `413`, `500` |
| `DELETE /api/v1/service/videos/{id}` | `204`: the video is taken down | `400`, `401`, `403`, `404`, `409`, `503`, `500` |
| `GET /api/v1/service/videos` | `200`, `application/json`, `Cache-Control: no-store`, a page of the videos of the user `?owner=` names | `400`, `401`, `403`, `503`, `500` |

**Visibility.** The `PATCH` takes the owner's body, `{"visibility":"private" | "unlisted" | "public" |
"room:<room id>"}` with `Content-Type: application/json`, at most 4 KiB, and sets it on any video
whatever its state, as its owner may. A room must list the video's owner at that moment (`403`
`not_member` otherwise): a room shares a video only while it lists the owner, so any other room
would share it with nobody.

**Many at once.** `PATCH /api/v1/service/videos` sets one visibility on up to 50 videos, for
publishing or unpublishing a course in one request. Its body is the single `PATCH`'s with the
videos' ids added, each at most once:

```json
{"visibility":"room:0192f3c4-...","videos":["0199950c-...","0199950d-..."]}
```

Each video is changed in turn, exactly as the single `PATCH` would change it, so some may change
and others not. Once the body is good the answer is `200`:

```json
{"videos":[{"id":"0199950c-...","title":"lesson 1","state":"ready","version":2,"duration_ms":6000,"progress":null,"visibility":"room:0192f3c4-..."}],"failed":[{"id":"0199950d-...","error":"not_member"}]}
```

`videos` lists the ones changed, in the order asked, as the single `PATCH` answers each one.
`failed` lists the others and why: `not_found` (no such video), `not_member` (the room does not
list its owner), or `unavailable` (the catalog could not be reached: retry that id).

**Takedown.** The `DELETE` has exactly the effect of the owner's
[delete](#deleting-a-video), for any user's video: gone for everyone at once, grants removed,
objects removed at the reaper's next pass, `204` again on a repeat. A video whose upload is in
progress is taken down too, its upload aborted, as the owner's delete does.

**Listing.** The `GET` lists one user's videos, every state included and deleted ones left out,
newest first (by creation, then by id, so the order is stable across pages). Its query:

| Parameter | Meaning |
|---|---|
| `owner` | Required: the user id, as the user's token names it, percent-encoded or not |
| `limit` | 1 to 200, default 50 |
| `after` | The `next` of the previous page, as given. Opaque: do not build one |

```json
{"owner":"auth0|123","videos":[{"id":"0199950c-...","title":"clip.mp4","state":"ready","version":2,"duration_ms":6000,"progress":null,"visibility":"unlisted","created_at":1759600000}],"next":"1759600000123456.0199950c-..."}
```

Each video is the [video object](#endpoints) as its owner sees it (`visibility`, `progress` while
processing, and `error_reason` when failed), with `created_at` in Unix seconds. A backend that
shows its users' uploads polls this page for their progress instead of one video at a time. With
the notification hook on, it also hears `video_ready` and `video_failed` as they happen
([Notifications](notifications.md#video_ready-and-video_failed)). `next` is `null` on the last page.
A user with no videos, or one who never signed in, has an empty list. Each page is read on its
own: a video created after the first page is not on the later ones, one deleted meanwhile is
left out, and none is listed twice.

Refusals carry a JSON body, `{"error":"<code>"}`:

| Status | `error` | Meaning |
|---|---|---|
| `400` | `bad_visibility` | `PATCH`: the body is not an object with one of the four forms; an empty body is `400` with no body |
| `400` | `bad_videos` | `PATCH` of many: `videos` is missing, empty, longer than 50, or has a malformed or repeated id |
| `400` | `bad_query` | `GET`: no `owner`, or `owner`, `limit` or `after` malformed, or one given twice |
| `403` | `forbidden` | Not a service token |
| `403` | `not_member` | `PATCH`: the room does not list the video's owner |
| `404` | `not_found` | No such video (deleted ones included), or a bad id |

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

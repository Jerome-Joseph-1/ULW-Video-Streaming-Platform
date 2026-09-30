# Uploads

A video file is uploaded in chunks to a resumable upload, then committed. The gateway streams
each chunk straight into the object store; it never holds a whole file. After the commit the
worker transcodes the video, and it becomes playable
([videos-and-playback.md](videos-and-playback.md)).

Every endpoint here needs a token ([auth.md](auth.md)). Every response carries `X-Request-Id`;
quote it when reporting a problem. Every error body is empty: the status code and the
`Upload-Offset`, `Retry-After` and `WWW-Authenticate` headers are the whole answer.

## Endpoints

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/connection.cpp -->

| Endpoint | Purpose | Success |
|---|---|---|
| `POST /api/v1/uploads` | Create an upload and its video | `201` with JSON |
| `PATCH /api/v1/uploads/{id}` | Append bytes at an offset | `204`, `Upload-Offset` |
| `HEAD /api/v1/uploads/{id}` | Ask where the upload stands | `204`, `Upload-Offset` |
| `POST /api/v1/uploads/{id}/commit` | Finish the upload and queue the transcode | `200` with JSON |
| `DELETE /api/v1/uploads/{id}` | Cancel the upload | `204` |

`{id}` is the `upload_id` from the create response: a lowercase canonical UUID. Anything else,
uppercase included, is `404`. Any other method on these paths is `405` with an `Allow` header.

### Create: `POST /api/v1/uploads`

<!-- apps/gateway/src/connection.cpp (on_head, start_create, on_created), core/src/content_type.cpp, core/src/video.cpp, core/include/core/models/upload.hpp -->

Request body, JSON, with a `Content-Length` of 1 to 4096 bytes:

```json
{"filename":"holiday.mp4","size_bytes":15893815,"content_type":"video/mp4"}
```

| Field | Rule |
|---|---|
| `filename` | String, 1 to 200 bytes of valid UTF-8, no control characters. Becomes the video's `title`. |
| `size_bytes` | Integer, 1 to 53687091200 (50 GiB). The exact size of the file; the upload is complete when this many bytes are durable. |
| `content_type` | A bare media type starting with `video/`, at most 127 characters, no parameters (`video/mp4; codecs=...` is refused), no wildcards. Case-insensitive. |

Unknown fields are ignored. With a bearer token the request's own `Content-Type` header is not
checked. With the cookie instead it must be `application/json` (parameters allowed), or the
answer is `403`: another site's page can make the browser send the cookie with a body it wrote,
but only under `text/plain` or a form's types.

Response `201`, `application/json`:

```json
{"video_id":"01a0ece4-69d0-781f-822e-f9f2e975cd5f","upload_id":"01a0ece4-69d0-7a49-805b-3846c3cf1039","chunk_size":8388608,"durable_offset":0}
```

| Field | Meaning |
|---|---|
| `video_id` | The video, for `GET /api/v1/videos/{id}` and playback |
| `upload_id` | The upload, for the other endpoints on this page |
| `chunk_size` | 8388608 (8 MiB). Send chunks of exactly this size, except the last. |
| `durable_offset` | Always 0 |

### Append: `PATCH /api/v1/uploads/{id}`

<!-- apps/gateway/src/connection.cpp (on_head, start_append, on_claimed, begin_append, on_durable, on_offset), http/include/http/request_parser.hpp, infra/storage/s3/src/s3_store.cpp (open) -->

| Header | Rule |
|---|---|
| `Upload-Offset` | Required. Decimal integer, no sign or junk. Must equal the upload's durable offset. |
| `Content-Length` | Required in practice: at most 16777216 (16 MiB), and at most `size_bytes - Upload-Offset`. A `PATCH` without it is an empty append. `Transfer-Encoding` is refused (`411`). |
| `Content-Type` | Not checked. `application/offset+octet-stream` is a good choice. |

The body is raw file bytes starting at `Upload-Offset`.

Response `204` with `Upload-Offset: <n>`, the new durable offset. It is reported only once those
bytes are durable in the store.

**Only whole chunks become durable.** The durable offset always sits on a multiple of
`chunk_size`, or at `size_bytes`. A body that ends partway through a chunk is accepted, but the
unfinished chunk is dropped and `Upload-Offset` in the answer stops at the last chunk boundary.
Sending 1 MiB at offset 0 answers `Upload-Offset: 0`; 12 MiB at offset 0 answers
`Upload-Offset: 8388608`. A `PATCH` may carry more than one chunk (up to 16 MiB), and a file up
to 16 MiB fits in one `PATCH`. Always continue from the offset the server returns, not from
where your bytes ended.

### Status: `HEAD /api/v1/uploads/{id}`

Response `204` with `Upload-Offset: <n>`: the durable offset, read from the store, so it is
right even after a `PATCH` whose response was lost. For a committed upload it is `size_bytes`.

### Commit: `POST /api/v1/uploads/{id}/commit`

<!-- apps/gateway/src/connection.cpp (on_found, on_committed), infra/postgres/src/upload_catalog.cpp (CommitUpload) -->

No body (`Content-Length` 0 or absent). Once all `size_bytes` are durable, the file is assembled
in the store, the video moves to `processing` and one transcode job is queued, in one
transaction.

Response `200`, `application/json`:

```json
{"video_id":"01a0ece4-69d0-781f-822e-f9f2e975cd5f","state":"processing"}
```

Commit is idempotent: repeating it after a success answers the same `200` and queues no second
job. The `state` in this answer is always `processing`, even on a repeat after the video is
`ready`; read the real state from `GET /api/v1/videos/{id}`.

### Cancel: `DELETE /api/v1/uploads/{id}`

Discards the bytes stored so far and marks the upload aborted. `204`, and `204` again on a repeat.
A committed upload cannot be cancelled (`409`). Afterwards `HEAD` answers `404` and `PATCH` and
commit answer `409`. The video stays in the state it had (`init` or `uploading`) and is never
playable.

## Resuming

<!-- apps/gateway/src/connection.cpp (on_claimed, on_offset), core/include/core/models/upload.hpp -->

The durable offset never moves backwards, and every offset the server has reported is a safe
place to continue. After any failure (network error, timeout, `408`, `409`, `5xx`, a crash of
the app):

1. `HEAD /api/v1/uploads/{id}` and read `Upload-Offset`.
2. `PATCH` from exactly that offset.
3. When `Upload-Offset` equals `size_bytes`, commit.

A `409` on `PATCH` carries the authoritative `Upload-Offset` too, so a client can skip the
`HEAD` and resume from it directly.

Only one `PATCH` per upload runs at a time. A second one while the first is in flight gets
`409` with the current offset; do not upload one file over parallel connections.

Upload lifetime: an upload must be committed within 6 days of its creation. The bucket drops
incomplete uploads after 7 days, after which `PATCH`, `HEAD` and commit fail and the upload has
to start again with a new create.

## Limits and admission

<!-- apps/gateway/src/gateway.hpp (Limits), apps/gateway/src/gateway.cpp (acquire_upload_slot, on_accept, admit_peer, charge_request, charge_upload_bytes), apps/gateway/src/connection.cpp (on_head, authenticate, start_append, on_timeout, check_body_rate, fail), apps/gateway/src/rate_limit.cpp (forwarded_client) -->

| Limit | Value | On breach |
|---|---|---|
| File size | 50 GiB | `400` at create |
| Chunk body per `PATCH` | 16 MiB | `413`, connection closed |
| Create body | 4 KiB | `413`, connection closed |
| Concurrent `PATCH`es per user, per gateway instance | 3 | `429`, `Retry-After: 5` |
| Concurrent `PATCH`es per gateway instance | 448 | `503`, `Retry-After: 5` |
| Requests per user, per gateway instance | 300 a minute, up to 300 at once | `429`, `Retry-After` until the next is allowed (at most 1 s once used up) |
| Upload bytes per user, per gateway instance | 100 GiB a day, refilled evenly (1.2 MiB/s) | `429`, `Retry-After` until this `PATCH`'s `Content-Length` fits |
| Requests in flight per client address before authentication, through Askedin's Envoy | 20 | `429`, `Retry-After: 1` |
| Connections per client address, direct | 20 open, 10 new a second | Reset at accept, no response |
| Connections per gateway instance | 448 | Closed at accept, no response |
| Request head | Complete within 10 s | Connection closed |
| Request target | 8 KiB | `400`, connection closed |
| Header fields | 16 KiB in all, at most 100 | `431`, connection closed |
| Body idle | Nothing received for 30 s | `408`, connection closed |
| Body rate | Under 8 KiB/s averaged over a 30 s window | `408`, connection closed |
| Store stalled | The store accepted nothing for 30 s | `503`, `Retry-After: 5` |
| Request lifetime | 6 h | Connection closed |
| Keep-alive | Idle connection closed after 10 s; at most 1000 requests per connection | Reconnect |

Admission counts chunk uploads only, and happens after authentication. A user's fourth upload
running at once is refused while other users go on; the whole instance being full refuses
everyone. Both answers carry `Retry-After: 5`.

The request and byte limits are token buckets: a user starts with the full allowance, spends one
token per authenticated request (any endpoint, playback included) and one per byte a `PATCH`
declares in `Content-Length`, charged when the `PATCH` is admitted, before its body is read,
with whatever the client then never sends given back when the request ends. A refused `PATCH`
reads nothing and holds no slot. The byte allowance is kept in each gateway's memory: a restart,
or a user going unseen while many others are active, starts it over full. Tokens come back
evenly, so a client that waits `Retry-After` seconds finds the request allowed. The byte
allowance is sized for two 50 GiB uploads a day; an ordinary uploader never meets the request
allowance (a 100 Mbit/s uplink sends 90 chunks a minute).

A client address is the connecting address, or behind Askedin's Envoy the address Envoy saw
(the gateway reads `X-Forwarded-For` from Envoy only; a client's own `X-Forwarded-For` entries
are ignored). An IPv6 client counts by its /64. Through Envoy the address limit covers only
requests not yet authenticated; once a token is verified the user's own limits apply instead,
so hundreds of users behind one carrier-grade NAT are not held to one address's 20.

Every limit applies per gateway process; production runs two behind Envoy, which spreads a
user's requests over both, so a user can have up to 3 chunks in flight on each, and up to twice
the request and byte allowances in all.

## Errors

<!-- apps/gateway/src/connection.cpp (fail, fail_storage, fail_catalog, on_found, on_claimed), http/src/request_parser.cpp -->

| Status | Headers | When | Client action |
|---|---|---|---|
| `400` | | Create: missing or invalid field, empty body, not JSON. `PATCH`: missing or malformed `Upload-Offset`, or a body longer than what remains. A body on `HEAD`, commit or `DELETE`. Malformed HTTP. | Fix the request; do not retry as is |
| `401` | `WWW-Authenticate` | No token (`Bearer`), or it fails verification (`Bearer error="invalid_token"`) | Refresh the token, retry once |
| `403` | | Create with the cookie and no `Content-Type: application/json` | Send the type |
| `404` | | Unknown or malformed id, another user's upload, unknown path. `HEAD` on a cancelled upload. | Stop; start a new upload if needed |
| `405` | `Allow` | Wrong method for the path | Fix the client |
| `408` | | Body idle for 30 s, or slower than 8 KiB/s | Resume from `HEAD` |
| `409` | `Upload-Offset` | `PATCH`: offset is not the durable offset, another `PATCH` on this upload is running, or the upload is committed or cancelled. Commit: not all bytes durable yet, or cancelled. `DELETE`: already committed. | Resume from the returned offset; commit once it equals `size_bytes`. If the upload is committed or cancelled, stop. |
| `411` | | `Transfer-Encoding` on a create or `PATCH` | Send `Content-Length` |
| `413` | | Create body over 4 KiB, or `PATCH` body over 16 MiB | Send smaller chunks |
| `429` | `Retry-After` | This user already has 3 chunk uploads running on this instance (`Retry-After: 5`); this user is over 300 requests a minute or 100 GiB a day; this client address has 20 requests in flight (`Retry-After: 1`) | Wait `Retry-After` seconds, then retry the same request |
| `431` | | Request head too large | Fix the client |
| `500` | | A bug or a misconfigured store | Report with `X-Request-Id`; retry later from `HEAD` |
| `501` | | Unknown HTTP method | Fix the client |
| `503` | `Retry-After: 5` | Instance full, store or database unavailable or throttling, the store stalled mid-chunk, or the key set is unreachable | Wait, then resume from `HEAD` |
| `505` | | Not HTTP/1.x | Use HTTP/1.1 |

After an error in the middle of a body the gateway closes the connection
(`Connection: close`); open a new one.

## Walkthrough

Run against a local gateway (built from this repository, backed by MinIO and Postgres, with a
development key set). `$GW` is the gateway's base URL and `$TOKEN` a token
([README](README.md#base-urls-and-environments) for real hosts). The responses shown are what that
run returned, trimmed to the relevant headers.

Create the upload:

```sh
SIZE=$(stat -c %s clip.mp4)        # 15893815
curl -sS -i -X POST "$GW/api/v1/uploads" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"filename\":\"clip.mp4\",\"size_bytes\":$SIZE,\"content_type\":\"video/mp4\"}"
```

```
HTTP/1.1 201 Created
Content-Type: application/json
X-Request-Id: 01a0ece4-69cc-773a-80d7-64a0eaa72345

{"video_id":"01a0ece4-69d0-781f-822e-f9f2e975cd5f","upload_id":"01a0ece4-69d0-7a49-805b-3846c3cf1039","chunk_size":8388608,"durable_offset":0}
```

Split the file into `chunk_size` pieces and send them in order:

```sh
U=01a0ece4-69d0-7a49-805b-3846c3cf1039
split -b 8388608 -d -a 2 clip.mp4 chunk.     # chunk.00 (8388608 B), chunk.01 (7505207 B)
curl -sS -i -X PATCH "$GW/api/v1/uploads/$U" -H "Authorization: Bearer $TOKEN" \
  -H 'Upload-Offset: 0' -H 'Content-Type: application/offset+octet-stream' \
  --data-binary @chunk.00
```

```
HTTP/1.1 204 No Content
Upload-Offset: 8388608
```

```sh
curl -sS -i -X PATCH "$GW/api/v1/uploads/$U" -H "Authorization: Bearer $TOKEN" \
  -H 'Upload-Offset: 8388608' -H 'Content-Type: application/offset+octet-stream' \
  --data-binary @chunk.01
```

```
HTTP/1.1 204 No Content
Upload-Offset: 15893815
```

A client that lost track asks the server, and an offset the server does not hold is refused with
the right one:

```sh
curl -sS -I "$GW/api/v1/uploads/$U" -H "Authorization: Bearer $TOKEN"
```

```
HTTP/1.1 204 No Content
Upload-Offset: 15893815
```

```sh
# Earlier in the same run, while 0 bytes were durable:
curl -sS -i -X PATCH "$GW/api/v1/uploads/$U" -H "Authorization: Bearer $TOKEN" \
  -H 'Upload-Offset: 4096' --data-binary @part1m
```

```
HTTP/1.1 409 Conflict
Connection: close
Upload-Offset: 0
```

Commit, twice to show it is idempotent:

```sh
curl -sS -i -X POST "$GW/api/v1/uploads/$U/commit" -H "Authorization: Bearer $TOKEN"
```

```
HTTP/1.1 200 OK
Content-Type: application/json

{"video_id":"01a0ece4-69d0-781f-822e-f9f2e975cd5f","state":"processing"}
```

The second commit answered the same `200` and body. After it, `PATCH` and `DELETE` answer
`409` with `Upload-Offset: 15893815`.

Poll the video until it is ready (5 s apart; this 12 s clip was ready at the second poll):

```sh
V=01a0ece4-69d0-781f-822e-f9f2e975cd5f
curl -sS "$GW/api/v1/videos/$V" -H "Authorization: Bearer $TOKEN"
```

```
{"id":"01a0ece4-69d0-781f-822e-f9f2e975cd5f","title":"clip.mp4","state":"processing","version":2,"duration_ms":null}
{"id":"01a0ece4-69d0-781f-822e-f9f2e975cd5f","title":"clip.mp4","state":"ready","version":3,"duration_ms":12000}
```

Play it: see [videos-and-playback.md](videos-and-playback.md) for the playlists this run
returned.

Other answers from the same run:

| Request | Answer |
|---|---|
| `PATCH` of 1 MiB at offset 0 of a fresh upload | `204`, `Upload-Offset: 0` |
| `PATCH` of 12 MiB at offset 0 | `204`, `Upload-Offset: 8388608` |
| `PATCH` of the whole 15.9 MB file at offset 0 | `204`, `Upload-Offset: 15893815` |
| `PATCH` of 16 MiB + 1 byte | `413`, `Connection: close` |
| `PATCH` without `Upload-Offset`, or `Upload-Offset: 0x10` | `400`, `Connection: close` |
| `PATCH` of 1 MiB at offset `size_bytes` | `400` |
| `PATCH` while another `PATCH` to the same upload is running | `409`, `Upload-Offset: 0` |
| A fourth concurrent `PATCH` by the same user | `429`, `Retry-After: 5` |
| Another user's `PATCH` at that moment | `204` |
| `HEAD` on another user's upload | `404` |
| `POST .../commit` with 0 bytes durable | `409`, `Upload-Offset: 0` |
| `POST .../commit` with a body | `400` |
| `DELETE` of an active upload, then again | `204`, `204` |
| `HEAD` after `DELETE` | `404` |
| `PATCH` or commit after `DELETE` | `409` |
| Create with `content_type` `image/png`, `video/mp4; codecs=x`, `size_bytes` 0, `"10"` or over 50 GiB, or a 201-byte `filename` | `400` |
| Create with a 5000-byte body | `413` |
| Create with `Transfer-Encoding: chunked` | `411` |
| No token; `x-user-id: user-1` and no token; a token with another issuer | `401` |
| `PUT /api/v1/uploads/{id}` | `405`, `Allow: HEAD, PATCH, DELETE` |

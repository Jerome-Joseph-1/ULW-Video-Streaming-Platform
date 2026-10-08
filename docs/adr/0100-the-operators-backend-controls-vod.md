# 0100. The operator's backend controls VOD: who uploads by a claim of the user's token, a video deleted at once in the catalog and purged by the reaper, and a service API to share, take down and list

Status: Accepted
Date: 2026-10-08

## Context

A product built on the platform (Askedin) wants its own backend to decide everything about the
videos its users make: who may upload at all, which videos are taken down, who sees what, and
what a user has uploaded. ADR-0097 gave the backend grants under a service token, and the owner a
visibility; four gaps remained:

- **Upload control.** Any signed-in user could create an upload. The product decides who may
  (a paid plan, a verified account, a moderator's say), and the platform must not hold that
  policy, only enforce what the product says.
- **Deletion.** Nothing deleted a video. Its owner could not take it back, and its source file
  and renditions stayed in the store for ever. `videos.deleted_at` existed since migration 0001,
  and every read since ADR-0097 already asks for it to be NULL, but nothing set it.
- **The backend's control of a video.** The backend could grant and revoke, not change a video's
  visibility nor take one down (abuse, a legal request, an account closed).
- **Listing.** The backend could not ask what a user has uploaded, in what state and shared how;
  ADR-0097 left listing out.

What had to be settled:

- **Where the upload policy lives.** The gateway sees a token and nothing of the product's
  accounts. The product already controls what its identity provider puts in a token, and ADR-0096
  and ADR-0097 already read one claim that way, for the service.
- **What a deletion removes, and when.** A video is a row (with its upload, jobs, renditions and
  grants hanging off it) and objects: `videos/<id>/raw` and `videos/<id>/hls/...`, written by the
  gateway or a live packager and by the worker. Removing a video's few hundred segments takes as
  many store round trips, each a blocking call the gateway would have to run on its offload
  pool. A transcode may be queued or running for the video.
- **What everyone else sees afterwards.** ADR-0097's rule: a video someone may not see is a `404`
  indistinguishable from a missing one; a deleted video must be exactly that, for everyone.
- **Idempotency.** A client or a backend that lost the answer repeats the request.
- **A listing that pages stably** while videos are created and deleted, with an index the table
  already has: building one on `videos` in a migration holds its writes for the build (ADR-0031,
  ADR-0097).

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Upload control by a claim of the user's token (`ULW_UPLOADER_CLAIM`, `ULW_UPLOADER_SCOPE`), read by the service claim's code | The product already decides what the token says; nothing to call per request; the same settings, parsing and matching as `ULW_SERVICE_*` | Accepted |
| An allowlist table the backend fills through the service API | Revocation at the next request, not the next token | Rejected: a second copy of the product's user state in the platform, one more read on every create, and a new API to keep in step; a token lives minutes |
| Ask the backend over HTTP on each create | Always current | Rejected: the product's availability becomes the upload path's, for a decision the token can carry |
| Reuse `ULW_LIVE_BROADCASTER_CLAIM`'s `<claim>=<value>` form | Already exists for streams | Rejected: it matches a value exactly, not a scope among space-separated ones, and the task is the service claim's shape; the two live side by side |
| Delete the row and the objects in the request | Simple to state | Rejected: hundreds of store calls on the gateway's request path, a request that fails halfway leaves a row without objects or objects without a row, and a running transcode writes renditions after |
| Mark the row (`deleted_at`) and queue it, in one statement; the reaper removes the objects, then the row | Every read already filters `deleted_at IS NULL`; the reaper already lists and removes objects, holds the store credentials for it and runs every 15 minutes (ADR-0049) | Accepted |
| Find what to purge by scanning `videos` for `deleted_at IS NOT NULL` | No new table | Rejected: a scan of the whole table every pass, or an index on `videos`, which a migration can only build holding writes |
| A worker job of a new kind to purge | The worker already takes jobs | Rejected: the worker has no delete permission and no listing, and a purge must not wait behind transcodes |
| Delete an upload in progress too | One call for everything | Rejected: the upload has a cancel already (`DELETE /api/v1/uploads/{id}`), its session and claim are the upload path's, and nobody else can see it; `409` `not_committed` says which call to make |
| Answer a repeat delete `404` | The video is gone | Rejected as the rule: a client that lost the `204` would see an error. A repeat answers `204` while the row waits for the reaper, and `404` after, which the docs ask clients to treat alike |
| The owner's `403` for a viewer on DELETE, as on PATCH | ADR-0097's rule: `404` for someone who may not see the video, `403` only to someone who may | Accepted |
| A separate service route per action (`/takedown`, `/visibility`) | Explicit | Rejected: `PATCH` and `DELETE` on the video's own service path say it, beside its `grants` |
| Listing by `created_at DESC, id DESC` with a cursor carrying both | Uses `videos_by_owner (owner_id, created_at DESC)` from migration 0001; the cursor survives the purge of the video it names | Accepted |
| A cursor that is the last video's id alone | Shorter | Rejected: the row it names may be purged between pages, and ordering by id needs an index the table lacks |

## Decision

- **Who may upload.** `ULW_UPLOADER_CLAIM` (default `scope`) and `ULW_UPLOADER_SCOPE` (default
  none), read by `infra::auth::read_uploader_claim`, which shares `read_service_claim`'s rules
  (one helper, the variables' names apart): the same claim names and values allowed and refused,
  no scope is off, a claim other than `scope` without a scope stops the process (exit 2).
  `ClaimRules::uploader_claim`/`uploader_value` carry them, and `Claims::may_upload` is the
  verdict, matched by `claim_holds` as the service's is (a string equal to it or listing it, an
  array with one, `true` for `true`); true where nothing is asked. `POST /api/v1/uploads` checks
  it first, before the body is looked at: `403` `{"error":"upload_not_allowed"}`. Only the create
  is checked; an upload already created goes on, and the token serves everything else. Off by
  default, so the shipped configuration behaves as before. The deployment carries
  `UPLOADER_CLAIM` and `UPLOADER_SCOPE` in config.env, empty.
- **Deletion, in the catalog.** `IUploadCatalog::delete_video(id, owner?)` runs one statement
  (`kDeleteVideo`): where the video is the owner's (or any, without an owner: the service), not
  deleted and committed (`processing`, `ready` or `failed`), it sets `deleted_at`, deletes its
  `video_grants`, fails a job still `queued` for it (`video deleted`) and inserts it into
  `video_purges`. A running job is left alone. The statement reports whether it deleted the row
  and how it found it: no row is `NotFound`; a row deleted already is success (idempotent); one
  in `init` or `uploading` is `Conflict`. From the commit on, every read (the access read, the
  owner's and the service's visibility, grants, listing) finds nothing, on every gateway, since
  each asks `deleted_at IS NULL` and nothing is cached (ADR-0097).
- **Deletion, over HTTP.** `DELETE /api/v1/videos/{id}`, no body: the owner's `delete_video`;
  `204` on success or repeat; `409` `not_committed` for an upload in progress. When it finds no
  video of the caller's, one access read (`find_video_for`, `access_of`) tells a viewer (`403`
  `forbidden`) from anyone else (`404` `not_found`), as the owner's PATCH does. A cookie DELETE is
  held to an allowed `Origin` (ADR-0078). `DELETE /api/v1/service/videos/{id}` is the same
  deletion for any video, under the service token (`404` for no such video, which the service
  may learn).
- **Purge, by the reaper.** Migration 0017 adds `video_purges (video_id PRIMARY KEY, deleted_at)`
  and its index by age; no foreign key, so nothing locks `videos`. The reaper's last phase,
  `IVideoPurges` on `PgUploadReaper`: `due(limit)` cancels any job queued again for a deleted
  video (a lapsed lease put back) and returns the longest-deleted videos with no job queued or
  running and not restored by an operator; for each, the reaper lists `videos/<id>/` and removes
  every key (a key gone already is fine), then `forget(id)` deletes the row (taking its upload,
  jobs, renditions and grants with it, `ON DELETE CASCADE`) and the queue entry in one statement,
  unless a job is queued or running again, when it answers false and the video comes due later.
  A failure leaves the video queued for the next pass. At most `videos_per_pass` (1,000) a pass,
  `batch` per call. Gauges: `reaper_videos_purged_last_run`, `reaper_videos_purge_failed_last_run`.
- **The service's visibility.** `PATCH /api/v1/service/videos/{id}` takes the owner's body and
  validation (`visibility_from_body`) and calls `set_visibility(id, nullopt, ...)`: any video, a
  room only one that lists the video's owner (`403` `not_member` otherwise), since a room shares
  a video only while it lists the owner (ADR-0097). The owner's statement became the same one,
  its room check now against `videos.owner_id`, which is the owner it names.
- **Listing.** `GET /api/v1/service/videos?owner=<user>&limit=&after=`: `owner` required
  (percent-encoded or not), `limit` 1 to 200 (default 50), `after` the previous page's `next`.
  `list_videos` reads `WHERE owner_id = $1 AND deleted_at IS NULL ORDER BY created_at DESC, id
  DESC LIMIT n + 1` through `videos_by_owner`, and from a cursor `(created_at, id) < (cursor)`.
  The cursor is `<created_at in Unix microseconds>.<video id>`, opaque to clients: exact (the
  database's own precision, carried as an integer) and independent of the row it came from.
  Every state is listed; each video is the owner's view of it plus `created_at` (seconds).
  `Cache-Control: no-store`. A malformed query is `400` `bad_query`, answered only after the
  token is known to be the service's, as for the grants' query.
- **Routes.** The three service routes sit under `/api/v1/service/videos`, which the shipped
  HTTPRoute already sends to the gateway (ADR-0097); the RUNBOOK's restriction applies to them.
- **Not done here:** a user's own listing of their videos; an undo through the API (an operator
  may restore a video before the reaper's pass, RUNBOOK); revoking a token's upload right before
  it expires; deleting a live stream's recording while the stream runs (its video only exists
  once the stream has ended, ADR-0092).

## Consequences

- The product holds the policy and the platform the mechanisms: who uploads is whatever its
  identity provider puts in the token, what is shared and taken down whatever its backend calls.
- Taking the upload right away lasts until the user's current token expires, minutes with a usual
  provider: the gateway caches nothing, but a token already issued keeps its claims.
- A deleted video is gone for every reader at the delete's commit; its storage costs until the
  reaper's next pass, at most 15 minutes, longer while a transcode still runs for it. Segment URLs
  a playlist handed out before stay valid until they expire, as after a revocation (ADR-0097).
- A worker that keeps writing after its lease lapsed, past the reaper's purge, can leave
  renditions no row names. The job's fence stops its catalog writes, not its store writes; the
  bucket's lifecycle rule does not cover finished objects. This needs a worker to outlive its
  lease and the reaper to run in that window; an operator can find such keys by listing
  `videos/` against `videos`.
- Uploads' error bodies were all empty; `upload_not_allowed` is the first with a JSON body, as the
  access API's refusals have (ADR-0097), so a client can tell it from a cross-site `403`.
- The reaper now deletes rows of `videos`: its role needs `DELETE` there and on `video_purges`,
  and `UPDATE` on `jobs`; the gateway's role, which it runs as, has them.
- A repeat delete answers `204` until the purge and `404` after; clients treat both as deleted.

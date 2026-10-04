# 0097. Videos are shared by a visibility their owner sets and by grants the operator's backend makes, checked against the catalog on every read

Status: Accepted
Date: 2026-10-04

## Context

Until now only a video's owner could see or play it: `on_video` answered `404` to anyone else,
exactly as for a video that does not exist (videos-and-playback.md). A product built on the
platform found that nobody could watch a video someone shared with them. The owner chose
"visibility plus grants":

- **A visibility per video**, one of: `private` (the owner only, as before); `room:<room id>`, the
  current members of a chat room, direct or group, as chat lists them in `chat_members`; or
  `unlisted`, any signed-in user who has the video's id.
- **Explicit grants** to single users, which the operator's backend makes and revokes through a
  service-authenticated API, whatever the visibility says.
- **The uploader sets the visibility**, among private, unlisted and the rooms they are a member
  of, through the user API.

What had to be settled:

- **Where the check runs.** A video is served on several paths: its metadata, the master and
  the media playlists, and the presigned segment URLs, which only a media playlist hands out
  (ADR-0002, ADR-0024). A recording of a live stream becomes a video like any upload (ADR-0092).
  Each path must apply the same rule, and none may hand out a URL the rule would refuse.
- **Where membership comes from.** `chat_members` is chat's table, but it is in the gateway's
  database: one database, one role, one set of migrations (operator-contract.md, RUNBOOK step 3,
  ADR-0031). Members change at any moment, by a user's command (ADR-0096) or an operator.
- **Multiple gateways, and latency.** Every gateway replica serves any request; a decision
  cached in one would outlive a removal made through another node.
- **What a service is.** The operator's backend has no user; it gets a token from the identity
  provider with the client-credentials grant. A parallel change gives chat a service API under
  the same mechanism, so the setting names and their parsing must be shared.
- **What the API reveals.** The rule that a hidden video is indistinguishable from a missing
  one must survive: nobody learns a video exists unless they may see it.
- **Uploads in progress.** A video in `init` or `uploading` is a file being written.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Check access in the gateway with one SQL statement per request, reading `videos`, `chat_members` and `video_grants` by their primary keys | Same database already; one round trip, the one the owner check already made; nothing to invalidate across replicas | Accepted |
| Cache membership in the gateway, invalidated by chat's `chat_members` notifications (ADR-0073) | Saves a lookup per playlist | Rejected: a LISTEN session per gateway and a cache to keep coherent across replicas for an indexed point lookup that costs microseconds; a removal must take effect at the next request |
| Ask chat over HTTP whether a user is in a room | Chat owns the table | Rejected: a cross-service call on every playlist, a new dependency for playback, and chat has no HTTP API (ADR-0096) |
| Signed share links (a capability token in the URL) | No grants table | Rejected: not what the owner chose; a link outlives its revocation, and a playlist URL leaks through players and logs |
| Visibility in a separate table | Keeps `videos` untouched | Rejected: one row per video either way; columns on `videos` come in the same read with nothing joined |
| Grants that override `init` and `uploading` | Simpler rule | Rejected: what exists of an upload is the uploader's until committed |
| Answer `403` to a user who may not see a video | Clearer to a client | Rejected: it tells them the id exists; `404` for both, and `403` only to someone who may see the video but not change it |
| A service role in a separate claim name per service | Each service decides | Rejected: one pair of settings, `ULW_SERVICE_CLAIM` (default `scope`) and `ULW_SERVICE_SCOPE`, parsed in the auth library for every service that takes service calls |
| Restrict live streams' viewing to the same rule | Consistent | Rejected for now: a stream has no visibility, and ADR-0059 makes it a broadcast; its recording, once a video, follows this ADR |

## Decision

- **Model** (`core/models/visibility.hpp`, `core/models/video_access.hpp`). `core::Visibility`
  is `private`, `unlisted` or `room:<room id>`; every video starts `private`. One pure rule,
  `core::access_of(video, viewer, facts)`, answers `Owner`, `Viewer` or `None`: the owner always;
  nobody else while the video is `init` or `uploading`; then a viewer if they hold a grant, if
  the video is unlisted, or if it is shared with a room `chat_members` lists them in.
- **One read per request.** `IUploadCatalog::find_video_for(video, viewer)` returns the video and
  the viewer's facts from one statement: the `videos` row by primary key, `EXISTS` on
  `chat_members` by its primary key `(room_id, user_id)`, and `EXISTS` on `video_grants` by its
  primary key `(video_id, user_id)`. Nothing is cached: a member taken off the room's list, or
  a grant revoked, loses the video at their next request, on every replica. The metadata route
  and both playlist routes call it and then `access_of`; `None` is the same empty `404` as a
  missing video. The media playlist, and so every presigned segment URL, is only built after
  the check passes. A recording's video is a video like any other, private until its
  broadcaster changes it.
- **What a viewer sees.** The video object without `visibility` and without `error_reason`
  (written for the owner); the owner's carries `visibility`. A viewer gets the same `409` the
  owner gets for a video that is not ready.
- **The owner's API.** `PATCH /api/v1/videos/{id}` with `{"visibility": ...}`. The gateway reads
  the video as above: `None` is `404` `not_found`, `Viewer` is `403` `forbidden`. For the owner,
  `set_visibility` updates the row only where `owner_id` matches and, for a room, only if
  `chat_members` lists the owner in it, in the same statement; when that changes nothing, a
  second read tells a room the owner is not in (`403` `not_member`) from a video that is not
  theirs (`404`). A cookie PATCH is held to the same rules as a cookie upload: an allowed
  `Origin` and `Content-Type: application/json` (ADR-0078).
- **Room semantics.** "Current members": the check reads `chat_members` at each request. A
  room's list is chat's to change (ADR-0096); the owner leaving the room takes nothing from the
  members (the owner may set the video private). A stream's live chat lists nobody, so a video
  shared with one is seen by nobody but its owner.
- **Grants.** `video_grants (video_id, user_id, granted_at)`, primary key `(video_id, user_id)`,
  user ids bytewise as in `chat_members`, deleted with their video. The service API:
  `POST` and `DELETE /api/v1/service/videos/{id}/grants/{user}` (`204`, idempotent; `{user}`
  percent-encoded or not) and `GET /api/v1/service/videos/{id}/grants` (`after`, `limit` 1 to
  1000, default 100; bytewise order; `next` cursor). A video that does not exist is `404`
  `not_found` to the service, which may know that. Grants are not capped: only the operator's
  backend makes them.
- **Service authentication.** A token is the service's when its claim `ULW_SERVICE_CLAIM` names
  (default `scope`) holds `ULW_SERVICE_SCOPE`: a string equal to it or listing it among
  space-separated values (OAuth's `scope`), an array with such a string, or `true` for `true`
  (`infra::auth::claim_holds`). `infra::auth::read_service_claim` validates both settings for
  every service; `ClaimRules::service_claim`/`service_value` carry them, and
  `Claims::is_service` is the verdict. Without `ULW_SERVICE_SCOPE` no token is a service's and
  the service routes answer `403` to all. A service token is otherwise an ordinary token: its
  subject is charged the per-user request limits like any user's.
- **Errors.** The new routes answer refusals with `{"error":"<code>"}`: `not_found`, `forbidden`,
  `not_member`, `bad_visibility`, `bad_user`, `bad_query`. A `404` on the existing read routes
  stays empty, as a missing video's does.
- **Migration 0016**, after 0015. One transaction: `video_grants` and its index by user built
  first, on a table with no rows; then one `ALTER TABLE videos` adding `visibility` (`text`,
  default `private`, a constant default that rewrites nothing), `visibility_room` (`uuid`, null)
  and two checks `NOT VALID` (the three kinds; a room exactly for `room`), all catalog changes
  that scan nothing, so videos' ACCESS EXCLUSIVE lock lasts only until the commit. No index on
  `videos`: every lookup goes by a primary key. A later migration validates the checks.
- **Live streams** keep their rule: any signed-in viewer may watch a stream's playlist
  (ADR-0059), and only the owner sees its recording through the stream status. Making a stream
  follow a visibility needs the stream to carry one; it is left for a later decision.
- **Not done here:** a listing of videos (there is no listing route; one would apply
  `access_of` per row, or filter in SQL by the same three tests), and notifying a viewer when a
  video is shared with them.

## Consequences

- A playlist request costs the same one round trip it did; the statement adds two primary-key
  probes. No state in any gateway, so replicas never disagree and a revocation is immediate.
- The gateway reads `chat_members`, so chat's table is now part of the gateway's contract: a
  change to its key or its meaning is a change to video access. The two share one database and
  role already; a deployment that split them would have to replicate membership first.
- The owner's PATCH may be refused for a room the owner left a moment ago; the client shows
  `not_member` and offers the rooms it lists (`rooms` on chat).
- Viewer counts include viewers other than the owner from now on: every master fetch is a view.
- Migration 0016 takes `videos`' SHARE ROW EXCLUSIVE lock for the foreign key, then its ACCESS
  EXCLUSIVE lock for the ALTER, both only until its commit; uploads' progress writes and the
  worker's transitions wait those moments, and reads wait only the last.
- A token from a provider that lets a user request any scope would let that user act as the
  service: `ULW_SERVICE_SCOPE` must be a value only the backend's client-credentials client is
  granted (auth.md).

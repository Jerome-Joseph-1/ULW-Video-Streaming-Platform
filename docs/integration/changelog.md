# Changelog

Changes to the Stable surfaces ([versioning.md](versioning.md)) that a client may have to act
on, newest first. An entry says what changed, who is affected and what to do.

## 2026-10-09: live streams are Stable

<!-- docs/integration/live.md, docs/integration/versioning.md, apps/gateway/src/connection.cpp (respond_stream), apps/live-packager/src/recorder.cpp, infra/postgres/src/live_recordings.cpp, tests/integration/live_recording_test.cpp, tests/e2e/live-publish.e2e.mjs -->

Live streams leave draft (M33). Nothing in the API changes today: what [live.md](live.md)
describes is what is served, and it is now kept under the compatibility rules in
[versioning.md](versioning.md).

| Before | Now |
|---|---|
| Starting, ticketing, going live and ending a stream (`/api/v1/live`) were a draft | Stable |
| Publishing over WHIP with the stream service's tickets was a draft | Stable |
| Watching (`GET /api/v1/live/{id}/index.m3u8`) was a draft | Stable |
| A stream's end and its recording were a draft until M33 | Stable: a stream that ended with media becomes exactly one video of its broadcaster's, `private`, titled `Live stream <stream id>`, that goes through `processing` to `ready` with the VOD ladder for the stream's resolution; the owner reads its id as `video_id` on `GET /api/v1/live/{id}`. A stream that ends with no media, or whose media cannot be read back, becomes no video, and `video_id` stays `null` |

What to do:

- **Clients:** nothing to change. After a stream ends, poll `GET /api/v1/live/{id}` for
  `video_id`, then follow the video as any other; treat a `video_id` that stays `null` as a
  stream with no recording. From now on a breaking change to live streams is announced here
  ahead of time; additive ones (a new field, an `ended_by` value) may arrive without notice, so
  ignore what you do not know.
- **Operators:** nothing to change.

## 2026-10-09: a processing video says how far along it is

<!-- apps/gateway/src/video_access.cpp (video_json), infra/postgres/src/upload_catalog.cpp (kFindVideoFor, kListVideos, kSetVisibility, decode_progress), infra/postgres/src/job_queue.cpp (kClaim), apps/worker/src/job_runner.cpp (KeeperProgress), docs/adr/0101-transcode-progress-on-the-video-object.md -->

Additive: the video object gains one field; nothing else changes.

| Before | Now |
|---|---|
| A video in `processing` said nothing more until `ready` or `failed` | The video object has `progress`: `{"stage":"queued"\|"transcoding","percent":0..99}` while `processing`, `null` in every other state, on `GET` and `PATCH /api/v1/videos/{id}`, `PATCH /api/v1/service/videos/{id}` and each video of `GET /api/v1/service/videos` ([Progress](videos-and-playback.md#progress)) |

What to do:

- **Clients and backends:** while polling a processing video, show `progress.percent`, and
  `progress.stage` `queued` as waiting. Treat `ready` as 100%. A retry sets it back to `queued` at
  0, so follow the value rather than assuming it only grows. A client that reads the object into
  a strict schema must allow the new field (versioning.md, rule 1).
- **Operators:** nothing; no migration, setting or grant. Deploy the worker with the gateway for
  publishing to report its share (90 to 99); an older worker's figure is capped at 99.

## 2026-10-08: the operator's backend controls VOD; owners delete videos

<!-- apps/gateway/src/connection.cpp (start_create, start_delete, start_service_route), apps/gateway/src/video_access.cpp (video_query, videos_json), infra/auth/src/service_claim.cpp (read_uploader_claim), infra/postgres/src/upload_catalog.cpp (kDeleteVideo, kListVideos), apps/reaper/src/reaper.cpp (purge_videos), migrations/0017_video_purges.sql, docs/adr/0100-the-operators-backend-controls-vod.md -->

Additive: with the new setting unset, every request a client sends today answers as before, but
a `DELETE` on a video, which was `405` and now deletes it. The platform gains the mechanisms;
when to use them is the product's policy.

| Before | Now |
|---|---|
| Any signed-in user could create an upload | Still, unless the operator sets `ULW_UPLOADER_SCOPE`: then only a token whose `ULW_UPLOADER_CLAIM` (default `scope`) holds it may, and any other gets `403` `{"error":"upload_not_allowed"}` on `POST /api/v1/uploads` ([uploads.md](uploads.md#who-may-upload)) |
| A video could not be deleted | Its owner deletes it with `DELETE /api/v1/videos/{id}` (`204`, again `204` on a repeat; `409` `not_committed` while the upload is in progress): from then on it is `404` to everyone, its grants are gone, and the reaper removes its source and renditions ([Deleting a video](videos-and-playback.md#deleting-a-video)) |
| The operator's backend could only grant and revoke | It also sets a video's visibility (`PATCH /api/v1/service/videos/{id}`, the owner's body and checks), takes a video down (`DELETE /api/v1/service/videos/{id}`, the owner's delete for any video) and lists a user's videos with their state and visibility (`GET /api/v1/service/videos?owner=<user>`, newest first, `limit` up to 200, a `next` cursor) ([Service API](videos-and-playback.md#service-api-visibility-takedown-and-listing)) |
| The reaper expired uploads and forgot unused rooms | It also purges deleted videos: every object under `videos/<id>/`, then the row; two new gauges, `reaper_videos_purged_last_run` and `reaper_videos_purge_failed_last_run` |

What to do:

- **Clients:** to let users delete their videos, call `DELETE /api/v1/videos/{id}` and treat
  `204` and a later `404` alike as deleted; a `409` `not_committed` is an upload to cancel instead.
  If the operator restricts uploads, handle `403` `upload_not_allowed` on create by not offering
  uploads to that user.
- **Backends:** use the service token you use for grants on the new routes; send the listing's
  `next` back as `after` unchanged.
- **Operators:** migration 0017 must run after 0016 (RUNBOOK); it only creates an empty table.
  Deploy the reaper with the gateway (same image), since only the new reaper purges deleted
  videos. To restrict uploads, set `UPLOADER_SCOPE` (and `UPLOADER_CLAIM` if your provider puts
  it elsewhere than `scope`) in config.env, and have your backend get that value into the tokens
  of the users who may upload.

## 2026-10-08: chat and calls are Stable

<!-- docs/integration/chat.md, docs/integration/calls.md, docs/integration/versioning.md, apps/chat/src/envelope.hpp, apps/chat/src/service_api.hpp, apps/chat/src/ring.hpp -->

Chat and calls leave draft. Nothing in the protocol changes today: what [chat.md](chat.md) and
[calls.md](calls.md) describe is what is served, and it is now kept under the compatibility rules
in [versioning.md](versioning.md).

| Before | Now |
|---|---|
| The chat WebSocket envelope (messages, acks, resume, history, member lists and the commands that change them, presence) was a draft until phase 2 | Stable |
| Chat's service API (`POST /service/v1/<operation>` on `ULW_SERVICE_PORT`) was a draft with it | Stable |
| Calls, 1:1 and group, were a draft until M26 | Stable |
| Live streams and end-to-end encryption were drafts | Still drafts ([live.md](live.md), [e2ee.md](e2ee.md)) |

What to do:

- **Clients:** nothing to change. From now on a breaking change to chat or calls is announced
  here ahead of time, as for the HTTP API; additive ones (a new field, message `type` or `error`
  reason) may arrive without notice, so ignore what you do not know.
- **Operators:** nothing to change. The service API's requests and answers, and the settings
  chat and calls read ([operator-contract.md](operator-contract.md)), are kept the same way.

## 2026-10-04: videos shared by visibility and by grants

<!-- core/src/video_access.cpp, apps/gateway/src/connection.cpp (on_video, start_update, start_service_route), apps/gateway/src/video_access.cpp, infra/auth/src/service_claim.cpp, migrations/0016_video_access.sql, docs/adr/0097-videos-shared-by-visibility-and-service-grants.md -->

A video can now be seen by others than its owner
([Who can see a video](videos-and-playback.md#who-can-see-a-video)). Additive: every existing
video is `private`, which answers exactly as before, and a client that never shares one sees no
change but one new field in its own video object.

| Before | Now |
|---|---|
| Only the owner could see or play a video | The owner sets its visibility: `private` (the default), `unlisted` (any signed-in user with the id) or `room:<room id>` (the room's current members), with `PATCH /api/v1/videos/{id}` |
| Nobody could give a single user a video | The operator's backend grants and revokes it, with `POST`/`DELETE /api/v1/service/videos/{id}/grants/{user}`, and lists grants with `GET /api/v1/service/videos/{id}/grants`, under a token whose `ULW_SERVICE_CLAIM` holds `ULW_SERVICE_SCOPE` |
| Nothing about a chat room reached videos | A video shared with a room is seen by its members while the room also lists the owner: leaving the room stops the share |
| The owner's video object had no `visibility` | It has one; anyone else who may see the video gets the object without `visibility` and without `error_reason` |
| Every master fetch was the owner's view | Every master fetch is still a view, now also by viewers other than the owner |

What to do:

- **Clients:** to share a video, PATCH its visibility (a room must be one the user is in; a
  `403` `not_member` otherwise) and pass its id to whoever should watch it. Anyone else's `404`
  still means "not yours to see". Ignore `visibility` if you do not use it.
- **Operators:** migration 0016 must run after 0015 (RUNBOOK); it is quick and scans nothing. To
  let your backend grant videos, set `SERVICE_SCOPE` (and `SERVICE_CLAIM` if your provider
  puts scopes elsewhere than `scope`) to a value only your backend's client-credentials client
  is granted, and `SERVICE_CLIENT_ID` to that client's id. The backend sends its token as
  `Authorization: Bearer`. The HTTPRoute now also sends `/api/v1/service/videos` to the gateway
  for a backend outside the cluster; the RUNBOOK shows how to restrict it.

## 2026-10-04: who may start a chat is the operator's; presence only between shared chats

<!-- apps/chat/src/membership.cpp (self_service), apps/chat/src/service_api.cpp, apps/chat/src/presence.cpp (watch, recheck), apps/chat/src/config.cpp, infra/postgres/src/message_sql.hpp (kSharedWith), docs/adr/0096-member-lists-changed-by-their-users.md -->

Breaking for clients that open chats themselves or watch people they share no chat with, unless
the operator turns self-service on. Lands with the member-list commands below, before either was
tagged.

| Before | Now |
|---|---|
| Any signed-in user could `open_direct` with, `create_group` with, or `add_members` anyone whose id they knew | Only with `ULW_CHAT_SELF_SERVICE=on`. Off, the default, the three answer `not_allowed`; `leave`, an admin's `remove_member`, `rooms` and `members` are unchanged |
| Operators listed members in SQL | Also, an operator's backend lists them through chat's [service API](chat.md#the-service-api), with a client-credentials token that `ULW_SERVICE_CLAIM` and `ULW_SERVICE_SCOPE` mark as the service's ([auth.md](auth.md#service-tokens)) |
| Anyone signed in could `watch` anyone | Only someone who shares a direct or group chat with the user; otherwise `not_shared`. A watch whose shared chat goes is dropped with an unasked `error` `not_shared` (or `unavailable`) naming the user |
| A `watch` was answered at once | After one read of the database: `watching` comes a round trip later; a watch refused is refused again from memory until either user is listed somewhere |
| A direct chat's pair could never be undone | The service API's `close_direct` unlists both (an unfriend, a block); users still cannot leave a direct chat themselves |

What to do:

- **Clients:** handle `not_allowed` (start conversations through your product, which calls the
  service API), and `not_shared` on `watch`, asked or not: show the user as unknown and stop
  watching. Watch people after you share a chat with them, and again after a `member` `added`
  if you want their presence.
- **Operators:** decide between self-service and the service API (RUNBOOK, step 10). A demo
  whose web client opens chats itself needs `ULW_CHAT_SELF_SERVICE=on`. For the service API, set
  up the backend's client in the identity provider, then `ULW_SERVICE_PORT` and
  `SERVICE_CLAIM`/`SERVICE_SCOPE` in config.env, `ULW_SERVICE_PORT` and `ULW_SERVICE_CLIENT_ID`
  on chat, a Service and a
  NetworkPolicy for the backend; watch
  `service_api_answers_total{result="forbidden"}`.

## 2026-10-04: group calls

<!-- apps/chat/src/call.cpp (CallHandler::callable, CallHandler::moderate), apps/chat/src/ring.cpp (Ringer::signal), apps/chat/src/envelope.cpp (call_move_of, write_call_event), docs/adr/0095-group-calls-from-the-rooms-owner-with-a-fenced-media-generation.md -->

A group chat now has a call ([calls.md](calls.md#group-calls)). Not breaking for 1:1 calls, with
one change: a member removed from a direct chat during its call ends it for both.

| Before | Now |
|---|---|
| `call` in a group chat was `not_callable` | A ticket to the group's call, of up to 8 devices; past that, `call_full` |
| Nothing put anyone out of a call | The caller's `call_expel`, or a removal from the chat, puts a member out: everyone hears `call_moved` and asks for a ticket again; the one put out gets `expelled` |
| `call_end` ended a call for both members | Unchanged for a direct chat; in a group call only the caller ends it for everyone, and the others `call_leave` |
| `call_ended` always had `by` | No `by` when nobody did it: a group call nobody was left in, or a call ended by a removal |
| A member removed from a direct chat stayed in its call | The call ends (`call_ended` with no `by`) |

What to do:

- **Clients:** to offer group calls, handle `call_left` and `call_moved` (on `call_moved`, ask
  for a ticket again unless you are `expelled`), the errors `expelled` and `call_full`, and a
  `call_ended` with no `by`. Clients that only make 1:1 calls need do nothing.

## 2026-10-03: clients open direct chats and manage group chats themselves

<!-- apps/chat/src/membership.cpp, apps/chat/src/envelope.cpp, migrations/0015_chat_membership.sql, docs/adr/0096-member-lists-changed-by-their-users.md -->

Additive to [chat.md](chat.md); nothing breaks for a client that does not use it. New commands on
the chat WebSocket: `open_direct`, `create_group`, `add_members`, `remove_member`, `leave`,
`rooms` and `members` ([Changing member lists](chat.md#changing-member-lists)), and an unasked
`member` frame when a member list you are on, or of a room you joined, changes.

| Before | Now |
|---|---|
| Members were listed by operators in the database | Users open direct chats and create groups; a group's admin adds and removes members; anyone leaves a group |
| A direct chat was any room id the operators listed two users in | `open_direct` names the pair's room: a version 8 id starting with `03`, the same whichever of the two asks. Rooms listed by operators keep working as before |
| A removal was told to the removed user's sockets in the room (`error` `not_member`) | The same, and then a `member` frame with `change` `removed` to every socket of that user and every socket in the room; additions are told likewise |
| Nothing told a group's members of a new admin | A `member` frame with `change` `promoted` (or `demoted`); `left` names who was promoted |
| No limit on what one account could list | `open_direct` and `create_group` of a new room answer `room_limit` once the user is in 1000 rooms; a group everyone left that holds messages is `gone` to a create under its id |
| A `join`'s `"kind"` was taken as given | For a room starting with `03` or `04` (version 8) the kind is the room's; a `"kind"` naming the other is `bad_room` |

What to do:

- **Clients:** to start a conversation, send `open_direct` and join the room it answers, instead
  of asking an operator. Ignore `member` frames you do not use; in end-to-end encrypted rooms,
  act on them as [chat.md](chat.md#changing-member-lists) says (commit the change in MLS).
- **Operators:** migration 0015 builds an index on `chat_members` that holds member changes (not
  joins) while it builds; deploy it off-peak (RUNBOOK). Watch
  `membership_refusals_total{reason="unavailable"}`. 0015 must run after 0011 to 0014; the
  release that carries it carries them (RUNBOOK).

## 2026-10-03: a live stream goes live and ends when its publisher does

<!-- apps/gateway/src/publisher_watch.cpp, apps/gateway/src/webhook_server.cpp, docs/adr/0093-livekit-webhooks-on-an-internal-listener.md -->

An addition; nothing that worked before changes. Where the media server reports to the
gateway (deployments with live streams on), a stream goes live as soon as its publisher's
first track arrives, and ends about 10 s after its publisher disconnects and does not come back
([live.md](live.md#starting-a-stream)).

| Before | Now |
|---|---|
| A stream went live only through `POST /api/v1/live/{id}/start` | It also goes live by itself when its publisher publishes; `start` still works and is harmless |
| A publisher that vanished left its stream `live` until its packager had exited and the sweep saw it (`finished`) | It is `ended` about 10 s after the publisher left, with `ended_by` `publisher_left` |
| `ended_by` was `owner`, `finished`, `failed` or `timeout` | It may also be `publisher_left` |

What to do:

- **Broadcaster clients:** nothing required. Encoders with a fixed token (OBS) no longer need a
  page to call `start` or `end`.
- **Clients reading `ended_by`:** accept `publisher_left`, and treat any value you do not know as
  an ended stream.
- **Operators:** LiveKit's configuration names the gateway's webhook listener, which no route
  exposes (RUNBOOK, step 9).

## 2026-10-03: live streams are started, taken live and ended through the gateway

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/live_streams.cpp, docs/adr/0092-the-stream-service-lives-in-the-gateway.md -->

An addition; nothing that worked before changes. A broadcaster's client now gets its publisher
tickets from five new endpoints ([live.md](live.md#starting-a-stream)): `POST /api/v1/live`,
`POST /api/v1/live/{id}/ticket`, `POST /api/v1/live/{id}/start`, `POST /api/v1/live/{id}/end`
and `GET /api/v1/live/{id}`.

| Before | Now |
|---|---|
| "How a client asks for its publisher ticket is not served yet" | `POST /api/v1/live` answers a stream with its first ticket, and `POST /api/v1/live/{id}/ticket` a fresh one |
| A published stream reached viewers only when an operator started its packager and relay | `POST /api/v1/live/{id}/start`, after the WHIP POST's `201`, does both |
| No endpoint mapped a stream to its recording | `GET /api/v1/live/{id}` answers the owner `video_id` |

What to do:

- **Broadcaster clients:** follow the flow in [live.md](live.md#starting-a-stream): create,
  POST the offer, `start`, a fresh ticket before every later WHIP request, then DELETE or `end`.
  One unfinished stream per user, a few started an hour (`429` past them), and, where the
  deployment names a broadcaster claim, only users holding it (`403` for others).
- **Viewer clients:** nothing changes for the playlist. `GET /api/v1/live/{id}` says whether a
  stream is `starting`, `live` or `ended`.
- **Operators:** the gateway needs LiveKit's API key and the packager settings, and on the
  cluster a service account that may create the packagers' Jobs (RUNBOOK, step 9).

## 2026-10-03: the subject claim is configured, and the audience is required

<!-- infra/auth/src/claims.cpp (subject_of), ops/src/dev_only.cpp (token_rules), docs/adr/0088-standalone-product.md -->

ULW no longer assumes one identity provider. A change to [auth.md](auth.md) for operators;
clients whose tokens carry `sub` are not affected.

| Before | Now |
|---|---|
| A token without `sub` was read by its `id` claim | The user is the claim `ULW_JWT_SUBJECT_CLAIM` names, `sub` by default; no other claim stands in, so a token without it is `401` |
| `JWT_AUDIENCE` defaulted to one provider's audience | Required whenever `JWKS_URL` is set: the gateway and chat exit `2` at startup without it. With a local development key set it defaults to `ulw-dev`, as `ulw_devtoken` mints |

What to do:

- **Operators:** set `JWT_AUDIENCE` to the audience your identity provider issues for ULW. If its
  tokens name the user in a claim other than `sub`, set `ULW_JWT_SUBJECT_CLAIM` to that claim
  before upgrading, or every such token is refused.

## 2026-10-03: a worker fault is no longer reported as the owner's bad file

<!-- infra/ffmpeg/src/exit_code.cpp (refine), apps/worker/src/job_runner.cpp (public_reason) -->

Not breaking; nothing to do. When the transcoder could not read the uploaded file or write its
output for a reason on our side (file permissions or a read-only mount on the worker), the
video went to `failed` at once with `error_reason` `the file could not be decoded as video`.
Such a job is now retried like the other transient worker faults, and only if every attempt
fails does the video go to `failed`, with `error_reason`
`the transcoder could not read its working files`. A file that really cannot be decoded is
reported as before. As [videos-and-playback.md](videos-and-playback.md) says, show
`error_reason` or log it, and do not parse it.

## 2026-09-30: the cookie is accepted only from trusted pages (security fix)

<!-- apps/gateway/src/connection.cpp (cookie_request_trusted), docs/adr/0078-the-cookie-is-believed-only-from-trusted-pages.md -->

A breaking change to the upload, video and playback endpoints. It ships under the security-fix
exception in [versioning.md](versioning.md) (item 3): no `/api/v2`, and no 30-day notice. It
applies only to requests that carry the auth cookie and no `Authorization` header. Bearer
clients are not affected.

| Before | Now |
|---|---|
| Any request with the cookie was believed | A request with the cookie from a page the gateway does not trust is `403`, before the token is checked ([auth.md](auth.md#cookies-and-other-sites)) |
| `POST`, `PATCH` and `DELETE` needed only the cookie | They must also send an `Origin` listed in `ULW_ALLOWED_ORIGINS` |
| `POST /api/v1/uploads` read any `Content-Type` | With the cookie it needs `Content-Type: application/json` |
| Cross-site `GET`s with the cookie were served and counted against the user's quota | Refused when the browser says `Sec-Fetch-Site: cross-site` (or `same-site` unless `ULW_ALLOW_SAME_SITE=1`) |
| A repeated `Origin` or `Sec-Fetch-Site` header was taken | `400` from the HTTP parser, for every client, bearer ones included |

What to do:

- **Web app teams:** send `Content-Type: application/json` on create. Check that the app's
  origin is in the gateway's `ULW_ALLOWED_ORIGINS`.
- **Operators:** set `ULW_ALLOWED_ORIGINS` to the web app's origin on each environment before
  deploying, and set `ULW_ALLOW_SAME_SITE=1` if the app is served from a sibling subdomain.
  Without these, cookie uploads are refused. Watch `cross_site_rejections_total` after the
  rollout.
  `ULW_ALLOWED_ORIGINS` takes `http://` only for `localhost`, `127.0.0.1` or `[::1]`, and no
  entry may name the default port (`:443`, `:80`); either stops the service at startup. The list
  admits only the gateway's own origin and, with `ULW_ALLOW_SAME_SITE=1`, sibling subdomains:
  a page on another site is refused by `Sec-Fetch-Site` whatever the list says.

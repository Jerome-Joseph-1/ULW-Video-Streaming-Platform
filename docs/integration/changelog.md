# Changelog

Changes to the Stable surfaces ([versioning.md](versioning.md)) that a client may have to act
on, newest first. An entry says what changed, who is affected and what to do.

## Unreleased

<!-- apps/chat/src/envelope.cpp (write_message, write_call_status), rt/src/wire.hpp (kVersion), apps/chat/src/ring.cpp (Ringer::occupied, Ringer::snapshot), apps/chat/src/call.cpp (CallHandler::counted, CallHandler::current), apps/chat/src/chat_service.cpp (ChatService::subscribe, ChatService::call_status), apps/chat/src/service_api.cpp (signal_call, call_status), docs/adr/0112-a-message-carries-the-time-its-owner-sequenced-it.md, docs/adr/0113-a-call-lasts-while-someone-is-in-its-media-room.md, docs/adr/0114-the-rooms-call-is-read-whatever-its-id.md, docs/adr/0115-a-call-says-whom-it-rings-and-who-moderates-it.md, docs/adr/0119-one-visibility-for-many-videos.md, apps/chat/src/video_relay.cpp (VideoRelay), migrations/0023_video_events.sql, docs/adr/0120-video-events-on-the-notification-hook.md, core/src/video_access.cpp (access_of_anonymous), migrations/0024_public_videos.sql, docs/adr/0121-a-public-video-plays-without-a-token.md -->

Additive (versioning rule 1), with calls that last as long as someone is in them.

| Before | Now |
|---|---|
| A `message` frame carried no time | It carries `at`, Unix milliseconds, the same live, resumed and from history ([Chat](chat.md#messages)) |
| A direct call rang on after its caller's page went away, and an answer could land in an empty call | The ring ends within about 20 s, `call_cancelled` with `reason: "caller_left"`; an answer then is `no_call` ([Calls](calls.md#ringing)) |
| An answered call was forgotten 2 minutes after its last ticket; a group ring that ran out ended the call | A call lasts while anyone is in its LiveKit room, and ends with `call_ended` (no `by`) when nobody is; a group call nobody answered stays joinable while its caller is in it |
| A second call within 2 minutes of an abandoned one joined it in silence | It ends the abandoned call (`call_ended`) and rings |
| A socket that joined mid-ring heard the ring at the next announcement, up to 15 s later | It hears `call_ringing` at once |
| The room's call could be read only by its id, from the backend | `call_status` with `room` alone, on the socket and the service API, answers the room's call and who is in it ([Calls](calls.md#after-a-reload)) |
| Only a decline could be made by the backend | `cancel_call`, `end_call` and `leave_call` too ([Chat](chat.md#the-service-api)) |
| Every first ticket rang every member (up to 32) | `call` may say `ring: false` (a call on at once, ringing nobody) or name the members to ring, and `max_people` ([Calls](calls.md)) |
| A group call's cap counted devices | It counts people too (`ULW_CALL_GROUP_PEOPLE`, `max_people`): another device of someone in the call always fits |
| Only a group call's caller could end it or put someone out | The group's admins can too |
| A decline said nothing more | `call_decline` and `decline_call` may give `reason` (`declined`, `busy`, `not_allowed`) and `text`, which `call_declined` carries on the sockets and the hook |
| An error answering a call command did not say which call | It names the `call`, and every call command may carry `rid`, echoed in its answer |
| A page centred on a message (jump to a quote) took two `history` requests | `history` with `around` ([Chat](chat.md#messages)) |
| A connection freed a place among its 64 rooms only by reconnecting | `part` stops following a room and frees its place, leaving the member list as it is ([Chat](chat.md#messages)) |
| The service API set one video's visibility per request | `PATCH /api/v1/service/videos` sets it on up to 50, and says which did not change and why ([Videos](videos-and-playback.md#service-api-visibility-takedown-and-listing)) |
| A backend learned that a video was ready or failed only by polling | With the notification hook on, `video_ready` and `video_failed` events ([Notifications](notifications.md#video_ready-and-video_failed)) |
| Every playlist needed a token | A `public` video plays for anyone with its id, signed in or not ([Videos](videos-and-playback.md#playing-without-a-token)) |
| A refused upgrade read as `1006` in a browser, like a network failure | `/rt?refusals=close` is refused on the socket, with close `4001`, `4003`, `4029` or `4503` ([Chat](chat.md#connecting)) |

What to do:

- **Clients:** show `at` instead of the time a message arrived. Treat `call_cancelled` with
  `reason: "caller_left"` as the caller gone, and `call_ended` with no `by` as the call over.
  Follow calls.md's advice for a page that unloads. Ask `call_status` after a reload.
- **Backends:** pass a leaving page's beacon on with `cancel_call` or `end_call`. Publish a
  video on `video_ready`, and keep polling the listing now and then for events you missed.
- **Operators:** chat's node channel moves to version 6: roll this release out with the chat
  Deployment set to `Recreate` (RUNBOOK.md, section 3). Migration 0023 adds `video_events` and a
  trigger on `videos`, and 0024 lets a video be `public`; both run with the gateway as usual.
  Roll every gateway out before anyone sets `public`: an older one answers `500` for such a
  video.

## 2026-10-15: notification events for members who are not connected (v1.2.0)

<!-- apps/chat/src/notify.cpp (Notifier), apps/chat/src/notify_sender.cpp (HookSender), apps/chat/src/session.cpp (device_of), apps/chat/src/service_api.cpp (decline_call, call_status), apps/chat/src/call.cpp (CallStatusRequest), migrations/0022_notify.sql, deploy/kubernetes/components/notifications/kustomization.yaml, docs/adr/0111-notification-events-for-members-not-connected.md -->

Additive (versioning rule 1): an opt-in hook, an optional query parameter on the socket's
upgrade, two service API operations, and a migration. Off unless the operator sets
`ULW_NOTIFY_URL`; nothing changes for a deployment that does not.

| Before | Now |
|---|---|
| A member with no socket open heard nothing of a call or a message | With `ULW_NOTIFY_URL` (components/notifications), every chat node POSTs signed events to the operator's receiver: `call_ringing` with each callee's connected devices, how the ring ended, and `message` naming who is owed word of it, never its content ([Notifications](notifications.md)) |
| A socket was its user's, no device named | `/rt?device=<uuid>` names it (optional; a malformed one is `400`) ([Chat](chat.md#connecting)) |
| The service API managed member lists | It also declines a call for a member (`decline_call`) and says whether one still rings (`call_status`) ([Chat](chat.md#the-service-api)) |

What to do:

- **Clients:** name the device on every socket, `/rt?device=<the id you send in call>`, so that
  a ring is not pushed to a device that already rings. Answering from a notification is what you
  do already: connect, `join`, and `call` with `answer` naming the ring.
- **Backends:** nothing unless your operator turns it on; then receive the events
  ([Notifications](notifications.md)).
- **Operators:** migration 0022 runs with the gateway as usual. To turn the hook on, RUNBOOK.md,
  step 7, "Notifications". During the rolling update a `call_status` asked of a room owned by a
  chat node still on v1.1 is `503` (retry).

## 2026-10-15: call tickets keep their `ice_servers` across chat's nodes (v1.1.1)

<!-- apps/chat/src/call.cpp (encode_answer, decode_answer, kLayout), apps/chat/src/layout.hpp, tests/call/direct-call.spec.mjs, docs/adr/0109-an-external-turn-service-as-an-alternative-to-stunner.md -->

A fix, no change to the documented surface: v1.1.0 documented `ice_servers` in call tickets, but
chat dropped them from every call ticket, since each one travels from the room's owner to the
asking node in a layout that had no place for them. Only external TURN mode was affected
(`ULW_TURN_MODE=external`); there, no call connected. Publisher tickets (the gateway's) always
carried them. No migration.

| Before | Now |
|---|---|
| v1.1.0, external mode: a call ticket had `url`, `token`, `expires_at` and `call`, and no `ice_servers` | Every call ticket carries `ice_servers` in external mode, as documented ([Calls](calls.md#the-flow)) |

What to do:

- **Clients:** nothing; pass `ice_servers` as the entry below says.
- **Operators:** run v1.1.1 or later before you turn external mode on. During the rolling update
  from v1.1.0, a call ask between an old and a new chat node is answered `unavailable` with
  `retry_after_ms` (clients retry), in either mode, until every node runs the new version.

## 2026-10-14: tickets may carry an external TURN service's ICE servers

<!-- infra/turn/src/relayed_sfu.cpp (make_relayed_sfu), infra/turn/src/config.cpp (load_config, append_ice_servers), apps/chat/src/envelope.cpp (write_ticket), apps/gateway/src/connection.cpp (append_ticket), deploy/kubernetes/components/external-turn/kustomization.yaml, docs/adr/0109-an-external-turn-service-as-an-alternative-to-stunner.md -->

Additive (versioning rule 1): a new optional field in call tickets and publisher tickets, present
only where the operator turns the external TURN mode on. Main only (v1.1 and later). No migration.

| Before | Now |
|---|---|
| A ticket was `url`, `token`, `expires_at` (and `call`); LiveKit's join response named STUNner as the relay | Unchanged by default. With `ULW_TURN_MODE=external` (components/external-turn), every ticket also carries `ice_servers`, the external TURN service's relays with credentials for that device ([Calls](calls.md#the-flow), [Publishing](live.md#publishing)) |

What to do:

- **Clients:** when a ticket has `ice_servers`, pass them as `rtcConfig: {iceServers:
  ticket.ice_servers, iceTransportPolicy: 'relay'}` to `room.connect` (calls) or to the
  `RTCPeerConnection` (WHIP publishers), and ask for a new ticket when a call outlives them.
  Do it now, before your operator switches: a client that ignores them cannot connect once
  the switch is made.
- **Operators:** nothing unless you turn it on (RUNBOOK.md, step 7, "External TURN").

## 2026-10-13: a create can carry an Idempotency-Key

<!-- apps/gateway/src/connection.cpp (on_head, start_create, on_keyed, on_created), infra/postgres/src/upload_catalog.cpp (kCreateUpload, kFindKeyedUpload), migrations/0021_upload_requests.sql, docs/adr/0108-a-creates-idempotency-key-names-one-upload.md -->

Additive (versioning rule 1): a new optional request header, and a migration. Main only (v1.1
and later); v0.2.x and v1.0.x do not have it.

| Before | Now |
|---|---|
| A create retried after its answer was lost made a second video of the same file | With `Idempotency-Key: <key>`, a retry answers the first create's `video_id` and `upload_id`; the same key for another file is `409` `idempotency_key_reused` ([Create](uploads.md#create-post-apiv1uploads)) |

What to do:

- **Clients:** send a key per file on `POST /api/v1/uploads` and keep it until the upload is
  committed; retry a create that timed out with the same key. Nothing changes without it.
- **Operators:** migration 0021 must run after 0020 (RUNBOOK); deploy the gateway, which runs
  migrations, as usual.
## 2026-10-13: a video mid-upload can be deleted, and cancelling an upload deletes its video

<!-- infra/postgres/src/upload_catalog.cpp (kDeleteVideo, kAbortUpload), infra/postgres/src/upload_reaper.cpp (kDuePurges), apps/reaper/src/reaper.cpp (purge_videos), apps/gateway/src/connection.cpp (on_deleted, on_service_deleted), docs/adr/0106-deleting-a-video-mid-upload-aborts-the-upload.md -->

A fix (versioning rule 3), in v0.2.1, v1.0.1 and later. No migration.

| Before | Now |
|---|---|
| `DELETE /api/v1/videos/{id}` and `DELETE /api/v1/service/videos/{id}` answered `409` `not_committed` for a video whose upload was in progress | Both answer `204`, again `204` on a repeat: the upload is aborted, the video is deleted and the reaper removes its staged parts ([Deleting a video](videos-and-playback.md#deleting-a-video)) |
| `DELETE /api/v1/uploads/{id}` left the video in `init` or `uploading`, listed and undeletable | The cancel deletes the video too: `404` everywhere, gone from `GET /api/v1/videos` ([Cancel](uploads.md#cancel-delete-apiv1uploadsid)) |
| | A `PATCH` or commit of an upload whose video was deleted answers `409` (`410` past its expiry, `404` once purged) and never brings the video back |

What to do:

- **The operator's backend:** take down abandoned uploads with `DELETE /api/v1/service/videos/{id}`;
  treat `204` and a later `404` as deleted. Videos a cancel left in `init` before this change go
  the same way, or by repeating the cancel.
- **Clients:** a delete no longer needs to know whether the upload finished; stop sending a cancel
  first. Stop handling `409` `not_committed`.
- **Operators:** nothing; no migration.

## 2026-10-11: who may watch a live stream is set, by the operator's backend or its owner

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/connection.cpp (start_live, act_on_stream, respond_stream), apps/gateway/src/live_streams.cpp, infra/postgres/src/live_streams.cpp, migrations/0020_live_stream_visibility.sql, docs/integration/live.md, docs/adr/0105-a-live-streams-visibility-is-set-by-the-product-and-enforced-here.md -->

Additive (versioning rule 1): a new route, a new field for the stream's owner, and a migration.
A stream nobody sets a visibility on is watched exactly as before.

| Before | Now |
|---|---|
| Anyone signed in who had a stream's id could watch it, and nothing could restrict that | The operator's backend (`PATCH /api/v1/service/live/{id}`) or the stream's owner (`PATCH /api/v1/live/{id}`) sets `unlisted` (the default, as before), `private` or `room:<room id>`; the playlist and `GET /api/v1/live/{id}` answer `404` to anyone it does not admit ([Who may watch](live.md#who-may-watch)) |
| The stream object had no `visibility` | The owner's carries `visibility`; nobody else's does |
| A stream's live chat admitted anyone signed in | It admits whoever may watch the stream, and answers anyone else `not_live`, as for a stream not on air ([chat.md](chat.md#a-streams-live-chat)) |

What to do:

- **The operator's backend:** apply the product's policy for streams through
  `PATCH /api/v1/service/live/{id}`, with the token it uses for videos' service API.
- **Clients:** nothing to keep today's behaviour. To keep a stream to a room or to its
  broadcaster, have it set before `start`. Allow the new `visibility` field on the owner's
  stream object.
- **Operators:** migration 0020 must run after 0019 (RUNBOOK); deploy the gateway, which runs
  migrations, as usual.

## 2026-10-11: a user lists their own videos

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/connection.cpp (start_own_listing), apps/gateway/src/video_access.cpp (own_video_query), docs/integration/videos-and-playback.md -->

Additive (versioning rule 1): a new route. Nothing a client sends today is answered
differently.

| Before | Now |
|---|---|
| A client could fetch a video only by its id; only the operator's backend could list a user's videos | `GET /api/v1/videos` lists the caller's own videos, newest first, a page at a time, as the service listing does for them ([Your videos](videos-and-playback.md#your-videos)) |

What to do:

- **Clients:** nothing. A "my uploads" screen can read this one page instead of keeping its own
  list of video ids, and see every upload's progress in one request.

## 2026-10-09: the worker publishes long videos in minutes, not tens of minutes

<!-- apps/worker/src/job_runner.cpp (publish, upload_all), apps/worker/src/config.cpp (ULW_PUBLISH_CONCURRENCY), infra/curl/src/http.cpp (Session), infra/storage/s3/src/s3_transfer.cpp (upload_session), deploy/kubernetes/base/video-worker/deployment.yaml, docs/adr/0103-the-worker-publishes-from-a-pool-on-kept-connections.md -->

Operators only; nothing a client sends or receives changes. A video becomes `ready` sooner after
its encode: the worker uploads its segments several at once, each upload thread keeping one
connection to the store.

| Before | Now |
|---|---|
| Segments uploaded one at a time, each on a new TCP and TLS connection: about 40 min to publish a 2 h video to R2 | `ULW_PUBLISH_CONCURRENCY` (1 to 32, default 8) segments at once, each thread on one connection for the whole publish; playlists and the master still last ([operator-contract.md](operator-contract.md#environment-by-name)) |
| No record of how long publishing took | Each job logs `publishing` (`files`, `concurrency`) and `published` (`files`, `bytes`, `wall_ms`, `mib_per_s`) |

What to do:

- **Operators:** nothing to keep the new default. The base worker Deployment sets
  `ULW_PUBLISH_CONCURRENCY: "8"`; set it to `1` for the previous one-at-a-time upload, or higher
  for a fast uplink to a distant store (RUNBOOK 4b). Expect up to that many concurrent PUTs per
  worker against your bucket.

## 2026-10-09: end-to-end encryption is Stable, with a key directory and every device a member

<!-- apps/chat/src/key_directory.cpp, apps/chat/src/envelope.hpp, apps/chat/src/envelope.cpp, infra/postgres/src/e2ee_directory.cpp, migrations/0018_e2ee_last_resort.sql, clients/web-mls/js/mls-room.js, docs/integration/e2ee.md, docs/adr/0102-the-key-directory-is-served-and-every-device-is-a-member.md -->

Additive (versioning rule 1): new message `type`s and `error` reasons on the chat socket, a new
migration, and new metrics. Nothing a client sends today is answered differently.

| Before | Now |
|---|---|
| [e2ee.md](e2ee.md) was a draft, and nothing served the key directory | Stable. The chat socket serves the directory: `register_device`, `publish_key_packages`, `retire_device`, `devices` and `claim_key_packages`, answered `device_registered`, `key_packages_published`, `device_retired`, `devices` and `key_packages`, and `replenish` sent unasked ([Key directory](e2ee.md#key-directory)) |
| A device out of key packages could not be invited | Each device keeps a last-resort package, handed out (and kept) once its single-use ones are gone |
| Each user had one device in a group, by convention; nothing found a user's other devices | Every device is its own member, `<user>/<device id>`: the group's adder lists every member's devices, claims a package of each device the group lacks and adds them in one commit, and removes devices retired or users no longer listed ([Multi-device](e2ee.md#multi-device)) |
| The browser client posted key packages to the room | It publishes them to the directory (`MlsDirectory`) and adds devices with `reconcile(users)`; key packages posted to the room are still accepted |
| | New `error` reasons for these commands: `unknown_device`, `device_retired`, `device_limit`, `key_packages_full`, and `not_shared`, `rate_limited`, `busy`, `too_large` as elsewhere |

What to do:

- **Clients:** mint a device id per device and use `<user>/<device id>` as its MLS identity;
  register and top up on every connect; have the first member's device call `reconcile` with
  the member list. Pages built on `mls-room.js` without a directory keep working unchanged.
- **Operators:** migration 0018 must run after 0017 (RUNBOOK); it only creates an empty table.
  Deploy the gateway (which runs migrations) before chat. Each chat node opens two more database
  sessions (`application_name` `ulw-e2ee`). New `e2ee_*` metrics ([operator-contract.md](operator-contract.md)).

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

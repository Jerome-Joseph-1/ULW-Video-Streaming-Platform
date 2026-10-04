# Changelog

Changes to the Stable surfaces ([versioning.md](versioning.md)) that a client may have to act
on, newest first. An entry says what changed, who is affected and what to do.

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
| A `watch` was answered at once | After one read of the database: `watching` comes a round trip later |

What to do:

- **Clients:** handle `not_allowed` (start conversations through your product, which calls the
  service API), and `not_shared` on `watch`, asked or not: show the user as unknown and stop
  watching. Watch people after you share a chat with them, and again after a `member` `added`
  if you want their presence.
- **Operators:** decide between self-service and the service API (RUNBOOK, step 10). A demo
  whose web client opens chats itself needs `ULW_CHAT_SELF_SERVICE=on`. For the service API, set
  up the backend's client in the identity provider, then `ULW_SERVICE_PORT` and
  `SERVICE_CLAIM`/`SERVICE_SCOPE` in config.env, `ULW_SERVICE_PORT` on chat, a Service and a
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

# Changelog

Changes to the Stable surfaces ([versioning.md](versioning.md)) that a client may have to act
on, newest first. An entry says what changed, who is affected and what to do.

## 2026-10-03: incoming calls ring browsers with no socket open (Web Push)

<!-- apps/chat/src/push.cpp, apps/chat/src/envelope.cpp (push_subscribe_of), migrations/0016_push_subscriptions.sql, docs/adr/0097-web-push-for-incoming-calls.md -->

Not breaking; new and optional. Where the operator sets a VAPID key, chat answers three new
socket commands (`push_key`, `push_subscribe`, `push_unsubscribe`), and a call that starts
ringing also sends a standard Web Push to every subscribed browser of each callee, carrying the
same `call_ringing` the socket does ([calls.md](calls.md#push-notifications)). Without the key the
commands are answered `push_disabled`.

What to do:

- **Web clients** that want calls to ring in the background: add a service worker and the flow in
  calls.md (subscribe on every start, show the notification tagged with the call, answer by
  opening the app and asking for a ticket, unsubscribe on sign-out).
- **Operators:** nothing, unless you want push. To turn it on, add `ULW_PUSH_VAPID_PRIVATE_KEY`
  and `ULW_PUSH_VAPID_SUBJECT` to `CHAT_SECRET` (RUNBOOK step 3 generates the key); both are
  optional in the base, and config.env does not change. Chat sends to the major browsers' push
  services over 443, which the shipped NetworkPolicy already allows. Migration 0016 adds
  `push_subscriptions`.

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

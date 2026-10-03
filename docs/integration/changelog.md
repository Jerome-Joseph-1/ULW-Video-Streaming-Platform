# Changelog

Changes to the Stable surfaces ([versioning.md](versioning.md)) that a client may have to act
on, newest first. An entry says what changed, who is affected and what to do.

## 2026-10-03: live streams are started, taken live and ended through the gateway

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/live_streams.cpp, docs/adr/0091-the-stream-service-lives-in-the-gateway.md -->

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
  One unfinished stream per user.
- **Viewer clients:** nothing changes for the playlist. `GET /api/v1/live/{id}` says whether a
  stream is `starting`, `live` or `ended`.
- **Operators:** the gateway needs LiveKit's API key and the packager settings, and on the
  cluster a service account that may create the packagers' Jobs (RUNBOOK, step 9).

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

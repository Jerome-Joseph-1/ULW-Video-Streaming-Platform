# 0078. The gateway believes the auth cookie only from pages it trusts

Status: Accepted, amended by 0088 (the origins are `ALLOWED_ORIGINS` in the operator's `config.env`)
Date: 2026-09-30

## Context

The gateway takes the Askedin token from `Authorization: Bearer` or from the `auth_token`
cookie (ADR-0018), so a browser app can upload and play without handling the token. A browser
attaches that cookie to requests any page makes, other sites' pages included, whenever the
cookie's `SameSite` allows it. `SameSite` is set by Askedin, not here, and a cookie without it
is sent on cross-site `POST`s for two minutes after it is set (Chrome's Lax+POST).

A review found what another site's page could do with the user's cookie:

- create uploads with titles of its choosing, with a `text/plain` `fetch` in `no-cors` mode or a
  `<form enctype="text/plain">`, neither of which needs a preflight;
- commit a started upload, with a body-less `POST` that needs no preflight either;
- burn the user's 300 requests a minute with a loop of `<img>` tags, since the quota is charged
  when the token is verified, before the answer is known.

`PATCH` and `DELETE` need a preflight, which the gateway never answers, but that is an accident
of the method list, not a defence. Chat already refuses a cookie socket whose `Origin` is not
in `ULW_ALLOWED_ORIGINS` (ADR-0036).

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Rely on `SameSite` | Nothing to build | Rejected: set outside this repo, and Lax still lets cross-site `POST`s through for two minutes |
| Require `Content-Type: application/json` on cookie requests | Forces a preflight for any request with a body | Kept only as a second layer on create: a commit has no body, and `GET`s are never preflighted |
| A CSRF token (double submit or synchronizer) | The textbook answer | Rejected: the gateway keeps no session to bind it to, and every client would need a new round trip |
| `Origin` on an allowlist for methods that change anything; `Sec-Fetch-Site` for every cookie request; both checked before the token | Browsers set both and no page can forge them; the allowlist is chat's, same shape and name; a refusal costs no quota | Accepted |
| Require `Origin` on `GET` too | Covers `<img>` from browsers without Fetch Metadata | Rejected: same-origin `GET`s, `<video>` and hls.js send none, so playback would break |

## Decision

For a request with the cookie and no `Authorization` header, checked in `on_head` before the
token is verified (`cookie_request_trusted` in `apps/gateway/src/connection.cpp`):

- `Sec-Fetch-Site`, when present, must be `same-origin` or `none`, or `same-site` when
  `ULW_ALLOW_SAME_SITE=1` says the app is served from a sibling subdomain.
- `Origin`, when present, must be listed exactly in `ULW_ALLOWED_ORIGINS`
  (`scheme://host[:port]`, comma separated, parsed as chat parses it: `http/src/origin.cpp`).
- Any method but `GET`, `HEAD` and `OPTIONS` must send `Origin`.
- `POST /api/v1/uploads` must also declare `Content-Type: application/json`.

Any failure is `403` with an empty body, counted in `cross_site_rejections_total`. It charges
no quota, and the answer is the same whether the token is valid or not. `Origin` and
`Sec-Fetch-Site` become single-valued headers in the parser: two copies are `400`. A bearer
token is not checked, because no other page can make the browser send one.

The gateway sends no CORS headers. Anything in front of it that adds them must list origins
explicitly and must never reflect `Origin` with credentials allowed (`docs/integration/auth.md`).

## Consequences

- A breaking change on endpoints marked Stable, shipped under versioning.md's security-fix
  exception and announced in `docs/integration/changelog.md`. A web client that uploads with the
  cookie needs its origin in `ULW_ALLOWED_ORIGINS` (the default is none) and must send the JSON
  type on create. The Askedin overlays set neither yet, and must before the cookie is used for
  uploads.
- Browsers from before 2023 send no `Sec-Fetch-Site`. From them a cross-site `GET` or `HEAD`
  with the cookie is still served and still charged, so the quota-burning attack remains for
  those users. Requests that change anything are protected in every browser, since all of them
  send `Origin` on those.
- An app on a sibling subdomain needs `ULW_ALLOW_SAME_SITE=1`. That also trusts every other
  sibling subdomain for `GET`s, which is why it is off by default.

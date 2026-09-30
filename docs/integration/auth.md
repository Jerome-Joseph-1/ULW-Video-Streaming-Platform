# Authentication

Every upload and playback endpoint, and the chat WebSocket, needs an Askedin access token. The
service keeps no users and issues no tokens of its own (ADR-0018): it verifies Askedin's JWTs
in-process against Askedin's published key set, and the user is whatever the token's subject
says.

## Where the token goes

<!-- infra/auth/src/token_extractor.cpp, infra/auth/include/infra/auth/token_extractor.hpp -->

| Source | Form | Notes |
|---|---|---|
| `Authorization` header | `Bearer <token>` | Scheme matched without regard to case, then one or more spaces. Wins over the cookie when both are present. |
| Cookie | `<cookie name>=<token>` | Name from `ULW_AUTH_COOKIE`: `auth_token` in prod, `auth_token_stage` on stage. The value may be wrapped in double quotes. |

A request is refused with `401` when:

- neither is present, or the cookie is present but empty (what a sign-out leaves);
- the `Authorization` header is not `Bearer <token>` (a malformed header is refused, not passed
  over for the cookie);
- the token holds any character outside `A-Z a-z 0-9 - _ .`;
- the token cookie appears twice (a sibling subdomain can plant a second cookie of the same
  name).

Two `Authorization` headers, or two `Cookie` headers, are refused earlier, by the HTTP parser,
with `400` and the connection closed.

A `401` carries `WWW-Authenticate` (RFC 6750): `Bearer` when no token was found, and
`Bearer error="invalid_token"` when one was found and refused. The body is empty.

A valid token that has made more than 300 requests in a minute on one gateway instance gets
`429` with `Retry-After` ([uploads.md](uploads.md#limits-and-admission)).

`x-user-id` and every other `x-user-*` header are ignored. They are never read, so they can
neither grant nor change an identity. There is no header a client or an in-cluster caller can
set to act as a user.

## Cookies and other sites

<!-- apps/gateway/src/connection.cpp (cookie_request_trusted, authenticate_head), apps/gateway/src/config.cpp, http/src/origin.cpp, docs/adr/0073-the-cookie-is-believed-only-from-trusted-pages.md -->

A browser attaches the cookie to requests that any page makes, including pages on other sites:
an `<img>`, a form, a `fetch` in `no-cors` mode. So the gateway accepts a cookie token only
from pages it trusts, and it tells them apart by two headers that no page can set. A request
with an `Authorization` header is not checked: other sites cannot make the browser send one.

| Request with the cookie and no `Authorization` | Answer |
|---|---|
| `Sec-Fetch-Site` present and not `same-origin` or `none` (or `same-site` with `ULW_ALLOW_SAME_SITE=1`) | `403` |
| `Origin` present and not listed exactly in `ULW_ALLOWED_ORIGINS` | `403` |
| `POST`, `PATCH`, `DELETE` or any other method except `GET`, `HEAD` and `OPTIONS`, with no `Origin` | `403` |
| `POST /api/v1/uploads` without `Content-Type: application/json` (parameters allowed) | `403` |

- Browsers send `Origin` on every `POST`, `PATCH` and `DELETE`, same-origin ones included. The
  web app's own origin must therefore be in `ULW_ALLOWED_ORIGINS` for it to upload with the
  cookie. With no list set, the cookie works only for `GET` and `HEAD` from the gateway's own
  origin.
- A same-origin `GET`, `<video>` and hls.js send no `Origin`, and playback needs none.
- Browsers released before 2023 send no `Sec-Fetch-Site`. From them, a cross-site `GET` or
  `HEAD` with the cookie is still answered.
- These checks run before the token is verified. A refused request gets `403` even when its
  token is invalid, and it charges none of the user's quota.
- Two `Origin` or two `Sec-Fetch-Site` headers are refused by the HTTP parser with `400`.

**CORS.** The gateway sends no CORS headers. If something in front of it adds them (for the
setup in [videos-and-playback.md](videos-and-playback.md)), it must name each allowed origin
explicitly. It must never echo the request's `Origin` back in
`Access-Control-Allow-Origin` together with `Access-Control-Allow-Credentials: true`: that would
let any site read the user's responses.

These rules took effect as a security fix on an endpoint marked Stable
([versioning.md](versioning.md), [changelog.md](changelog.md)).

## What a token must be

<!-- infra/auth/src/jws.cpp, infra/auth/src/jws.hpp, infra/auth/src/jwk.cpp, infra/auth/src/claims.cpp, infra/auth/src/claims.hpp -->

| Rule | Value |
|---|---|
| Format | Compact JWS: exactly three base64url parts separated by two dots, at most 8 KiB in all |
| Header `alg` | `RS256`, `PS256`, `ES256` or `EdDSA`. Anything else, `none` included, is refused. |
| Header `kid` | Required, 1 to 256 bytes, and must name a key in the key set |
| Header `crit` | Must be absent |
| Algorithm vs key | The key decides. An RSA key (2048 to 8192 bits) verifies `RS256`/`PS256`, an EC P-256 key `ES256`, an OKP Ed25519 key `EdDSA`. A key that publishes its own `alg` accepts only that one. A token whose `alg` does not fit its key is refused. |
| Keys used | Only keys with `use` absent or `sig`, and `key_ops` absent or containing `verify` |
| `iss` | Must equal `JWT_ISSUER` exactly |
| `aud` | A string equal to `JWT_AUDIENCE`, or an array of strings containing it. Default `askedin-platform`. |
| `exp` | Required, NumericDate. Refused once `now - 60 s >= exp`. |
| `nbf` | Optional. Refused while `now + 60 s < nbf`. |
| Clock skew | 60 s on both `exp` and `nbf` |
| `iat` | Not consulted. Present or not, in the future or not, of any type, it changes nothing; only `exp` and `nbf` bound a token's lifetime. |
| `email` | Optional string, at most 254 bytes, no control characters. `null` counts as absent. |

The signature is checked before any claim is read.

## How the user id is derived

<!-- infra/auth/src/claims.cpp (subject_of), core/src/ids.cpp (UserId::parse) -->

1. `sub`, if present. It must be a string.
2. Otherwise `id`, as a string or as a non-negative integer (taken in decimal, `42` becomes `"42"`).
3. Neither present: refused.

The result must be 1 to 128 characters from `A-Z a-z 0-9 . _ : @ | + -`; anything else is
refused rather than cleaned up. This string is the owner of every video the user uploads, and
it is compared byte for byte. Two tokens with different subjects are two different users.

## Key set

<!-- apps/gateway/src/config.cpp (load_auth), infra/auth/src/jwks_verifier.cpp, infra/auth/include/infra/auth/jwks_verifier.hpp -->

| Setting | Meaning |
|---|---|
| `JWKS_URL` | Askedin's JWK set. Must be `https://`; the process refuses to start otherwise. |
| `JWT_ISSUER` | Required. |
| `JWT_AUDIENCE` | Optional, default `askedin-platform`. |
| `ULW_AUTH_COOKIE` | Optional, default `auth_token`. |
| `ULW_DEV_JWKS_FILE` | Development only: a local Ed25519 key set instead of `JWKS_URL`. Setting both is a startup error. The first log line prints `keys=DEVELOPMENT <file>` so it cannot go unnoticed. |

Caching and refresh:

- The key set is fetched in the background and refetched every 15 minutes. A key withdrawn from
  the set keeps verifying until the next successful refetch.
- A failed fetch is retried with backoff from 1 s, doubling, up to 1 minute.
- A token whose `kid` is not in the cached set waits for one refetch, shared by every request
  that arrives meanwhile. If the key is still missing afterwards, the token is refused, and that
  `kid` is refused without another fetch for 60 s. Fetches triggered by unseen `kid`s are at
  least 10 s apart.
- A verified token is remembered by digest for up to 15 minutes, never past its `exp`.

So a newly rotated-in key is accepted within one fetch of first use, provided Askedin publishes
it before issuing tokens with it.

## Rejections

<!-- apps/gateway/src/connection.cpp (on_head, authenticate), apps/chat/src/session.cpp (route, answer_request) -->

| Case | Gateway (HTTP) | Chat (`GET /rt` upgrade) |
|---|---|---|
| No token, malformed header, duplicate token cookie | `401` | `401` |
| Any verification failure: bad signature, unknown `kid`, expired, wrong `iss` or `aud`, missing or malformed subject | `401` | `401` |
| The key set cannot be fetched and no cached key fits the token | `503` with `Retry-After: 5` | `503` |
| Cookie token on a socket whose `Origin` is not allowed (see [chat.md](chat.md)) | n/a | `403` |
| Cookie token from a page not trusted, or a cookie create without `Content-Type: application/json` ([Cookies and other sites](#cookies-and-other-sites)). Checked before the token, so `403` rather than `401` | `403` | n/a |

Every rejection has an empty body (`Content-Length: 0`) and no `WWW-Authenticate` header. The
gateway's responses carry `X-Request-Id`; quote it when reporting a problem. The reason for a
401 is not sent to the client.

Client action:

- `401`: get a fresh token from Askedin (sign in again, or refresh) and retry once. A second
  `401` with a fresh token means a configuration mismatch (issuer, audience, key set); report it
  with the request id.
- `503`: retry after the `Retry-After` delay. Do not sign the user out; the token may be fine.

Authentication happens after routing and after the per-route header checks, so an unknown path
answers `404`, a wrong method `405`, and a malformed `Upload-Offset` `400`, whatever the token.

`/api/v1/healthz`, `/api/v1/readyz` and `/metrics` need no token.

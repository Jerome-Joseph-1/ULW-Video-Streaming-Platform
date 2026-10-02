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

A gateway `401` carries `WWW-Authenticate` (RFC 6750): `Bearer` when no token could be read
from the request, and `Bearer error="invalid_token"` when one was read and failed verification;
chat's does not ([Rejections](#rejections)). The body is empty.

A valid token that has made more than 300 requests in a minute on one gateway instance gets
`429` with `Retry-After` ([uploads.md](uploads.md#limits-and-admission)).

`x-user-id` and every other `x-user-*` header are ignored. They are never read, so they can
neither grant nor change an identity. There is no header a client or an in-cluster caller can
set to act as a user.

## Cookies and other sites

<!-- apps/gateway/src/connection.cpp (cookie_request_trusted, authenticate_head), apps/gateway/src/config.cpp, http/src/origin.cpp, docs/adr/0078-the-cookie-is-believed-only-from-trusted-pages.md -->

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
- `ULW_ALLOWED_ORIGINS` entries are `scheme://host[:port]`, lowercase, exactly as a browser
  writes `Origin`. `http://` is accepted only for `localhost`, `127.0.0.1` and `[::1]` (a dev
  server); any other `http://` entry stops the gateway at startup. An entry naming the scheme's
  default port (`https://app.example:443`, `http://localhost:80`) is refused too: browsers leave
  it out of `Origin`, so it could never match.
- `Sec-Fetch-Site` is checked first, so the list admits only same-origin pages, and same-site
  ones with `ULW_ALLOW_SAME_SITE=1`. Listing a page on another site does not let it use the
  cookie from a current browser.
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
| Header `alg` | `RS256`, `PS256`, `ES256` or `EdDSA`. Anything else, `none` included, is refused. Askedin signs with `RS256` only ([Askedin](#askedin)). |
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

<!-- apps/gateway/src/config.cpp (load_auth), infra/auth/src/jwks_verifier.cpp, infra/auth/include/infra/auth/jwks_verifier.hpp, apps/gateway/src/gateway.cpp and apps/chat/src/chat.cpp (on_signal, drop_auth_caches) -->

| Setting | Meaning |
|---|---|
| `JWKS_URL` | Askedin's JWK set. Must be `https://`; the process refuses to start otherwise. Askedin's values: [Askedin](#askedin). |
| `JWT_ISSUER` | Required. Askedin's values: [Askedin](#askedin). |
| `JWT_AUDIENCE` | Optional, default `askedin-platform`. |
| `ULW_AUTH_COOKIE` | Optional, default `auth_token`. |
| `ULW_JWKS_MAX_STALE_HOURS` | Optional, 1 to 168, default 24: how long keys stay trusted while every refetch fails (below). |
| `ULW_DEV_JWKS_FILE` | Development only: a local Ed25519 key set instead of `JWKS_URL`. Setting both is a startup error. It is refused (exit 2) unless `ULW_DEV_MODE=1`, and refused regardless inside a Kubernetes pod (`KUBERNETES_SERVICE_HOST` set, as the kubelet does in every container), so a key set left in a real deployment's configuration stops the process instead of being trusted. The gateway and chat server both apply this. The first log line prints `keys=DEVELOPMENT <file>` so it cannot go unnoticed. |
| `ULW_DEV_MODE` | `0` or `1`, default `0`. `1` says this is a development run, which development-only settings such as `ULW_DEV_JWKS_FILE` need. |

Caching and refresh:

- The key set is fetched in the background and refetched every 15 minutes. A key withdrawn from
  the set keeps verifying until the next successful refetch.
- Keys are trusted for at most `ULW_JWKS_MAX_STALE_HOURS` (1 to 168, default 24) after the last
  successful fetch. Past that, while every refetch fails, the keys and every cached verdict are
  dropped and each token is refused as if the key set were unavailable, until a fetch succeeds:
  a key Askedin withdraws while its JWKS cannot be reached stops verifying within a day. The
  process logs `jwks keys expired` at error level once, and the `jwks_keys_expired` gauge (gateway
  and chat `/metrics`) reads 1 while it lasts; alert on it. The default is a day because refetches
  are 15 minutes apart and retried every minute, so an outage that long has had 1,440 retries and
  a night for someone to notice, while ADR-0018 accepts cached keys only for riding out outages,
  not indefinitely.
- A failed fetch is retried with backoff from 1 s, doubling, up to 1 minute.
- A token whose `kid` is not in the cached set waits for one refetch, shared by every request
  that arrives meanwhile. If the key is still missing afterwards, the token is refused, and that
  `kid` is refused without another fetch for 60 s. Fetches triggered by unseen `kid`s are at
  least 10 s apart.
- A verified token is remembered by digest for up to 15 minutes, never past its `exp`.
- A key withdrawn from the set therefore goes on verifying, and the tokens it verified go on
  being accepted, until the next successful refetch: up to 15 minutes, or sooner if a token with
  an unseen `kid` triggers a fetch first.
- SIGHUP drops the cached key set, every remembered verified token and every remembered unknown
  `kid` (gateway and chat; ADR-0079). A fetch starts at once, replacing one in flight, and
  requests that arrive meanwhile wait on it as on an unseen `kid`; a token whose key left the set
  is then refused with `401`. Each drop logs `auth caches dropped` at info level and counts in
  `auth_cache_drops_total` on `/metrics`. If the key set cannot be fetched then, every token is
  answered `503` until a retry succeeds (from 1 s, as above), so do not send SIGHUP while the
  JWKS is down. Chat sockets already open are not closed; they run to their token's `exp`.

So a newly rotated-in key is accepted within one fetch of first use, provided Askedin publishes
it before issuing tokens with it, and a withdrawn key stops verifying at the next SIGHUP.

## Askedin

<!-- tests/unit/auth/jwks_verifier_test.cpp (askedin_token), deploy/askedin/overlays/*/video-gateway/deployment.yaml -->

What Askedin's auth-service issues and publishes, as its owner set it out on 2026-10-03 from its
code (commit `ebd9b2ab`) and the live hosts, and the settings ULW needs for it.

| Setting | Stage | Prod |
|---|---|---|
| `JWKS_URL` | `https://auth-stage.askedin.com/.well-known/jwks.json` | `https://auth.askedin.com/.well-known/jwks.json` |
| `JWT_ISSUER` | `https://auth-stage.askedin.com/auth`, **unconfirmed**: the value in Askedin's deployment template, not yet read from the live secret (below) | `https://auth.askedin.com` exactly: no path, no trailing slash (confirmed 2026-10-03) |
| `JWT_AUDIENCE` | `askedin-platform` (the default) | `askedin-platform` (the default) |
| `ULW_AUTH_COOKIE` | `auth_token_stage` | `auth_token` |
| `ULW_ALLOWED_ORIGINS` (chat socket) | `https://stage.askedin.com` | `https://askedin.com,https://www.askedin.com` |

These origins were given for chat's socket. The gateway reads the same variable for cookie
writes ([Cookies and other sites](#cookies-and-other-sites)); its overlays do not set it yet.

`iss` is compared byte for byte, so a wrong `JWT_ISSUER` refuses every token with `401`. Until
the stage value is read from the live secret, stage's `JWT_ISSUER` stays in the gateway's
secret, not in its overlay. Whoever has access to the stage cluster confirms it with:

```sh
kubectl -n apps-stage get secret auth-service-secrets -o jsonpath='{.data.ISSUER}' | base64 -d
```

Do not use:

- `https://askedin.com/.well-known/jwks.json`: it answers an HTML page, not a key set, so every
  fetch fails and no token ever verifies.
- The auth-service's in-cluster `http://` URL: `JWKS_URL` must be `https://`, and the process
  refuses to start otherwise.
- OIDC discovery: `/.well-known/openid-configuration` answers `404`. ULW never reads it; it needs
  `JWKS_URL` and `JWT_ISSUER` set explicitly.

The key set is `{"keys":[...]}` as `application/json` with `Cache-Control: public, max-age=3600`,
and lists only active keys, normally one. ULW does not read the header; it refetches every 15
minutes as above.

### Askedin's tokens

| Item | What Askedin sends | What ULW does with it |
|---|---|---|
| Signature | `RS256` (RSASSA-PKCS1-v1_5 with SHA-256), one RSA-2048 key per environment. Nothing is signed with `PS256`, `ES256` or `EdDSA`. | Verifies against the key named by `kid` |
| Header | `{"alg":"RS256","kid":...,"typ":"JWT"}`, nothing else | `typ` is not read |
| `iss`, `aud` | As in the table above; `aud` is an array of one | Checked as in [What a token must be](#what-a-token-must-be) |
| `sub` | The user id, a lowercase canonical UUID, stable for the user | The user, byte for byte |
| `uid` | Equal to `sub` | Not read |
| `exp` | `iat` + 3600: tokens live an hour | Checked, 60 s skew |
| `iat` | Set | Not read |
| `nbf` | Not set | Checked only when present |
| `jti` | Set, but **equal to the `kid`**: the same for every token under a key, so not a token id | Not read. Nothing may use it as a token id, a replay key or a cache key; the verdict cache keys on the token's SHA-256 |
| `email` | Optional; never `null` | Optional string |
| `tid`, `perms`, `sid` | Set | Not read |

The same key also signs two kinds of token that must never authenticate at ULW. Only the audience
tells them apart from an access token, and under `JWT_AUDIENCE=askedin-platform` both are refused
with `401` (`WrongAudience`):

| Token | `aud` | `typ` claim | Lifetime |
|---|---|---|---|
| 2FA challenge | `askedin-2fa` | `2fa_challenge` | 5 minutes |
| PAT internal swap | `askedin-pat` | `pat` | 2 minutes at most |

Never set `JWT_AUDIENCE` to either of these. A test
(`JwksVerifierTest.AskedinsTwoFactorAndPatTokensAreRefusedForTheirAudience`) keeps both refused.

### Key rotation

Askedin rotates with no overlap: one transaction creates the new key and deactivates the old
one, and the old `kid` leaves the JWKS at once. ULW, left alone, goes on accepting tokens signed
with the old key for up to 15 minutes after that, from the cached key set and the remembered
verified tokens (Caching and refresh, above).

So Askedin's rotation runbook, which restarts the services that verify its tokens, must also
reach ULW once the rotation has committed, in the rotated environment: SIGHUP every
`gateway_server` and `chat_server` process (deploy/askedin/RUNBOOK.md, "Signing key rotation"),
or restart them. Afterwards each pod's `auth_cache_drops_total` has gone up by one, and a token
under the old key gets `401`.

## Rejections

<!-- apps/gateway/src/connection.cpp (on_head, authenticate, fail), http/src/response.cpp (write_response_head), apps/chat/src/session.cpp (route, answer_request) -->

| Case | Gateway (HTTP) | Chat (`GET /rt` upgrade) |
|---|---|---|
| No token, malformed header, a token with characters outside `A-Z a-z 0-9 - _ .`, duplicate token cookie | `401` with `WWW-Authenticate: Bearer` | `401` |
| Any verification failure: bad signature, unknown `kid`, expired, wrong `iss` or `aud`, missing or malformed subject | `401` with `WWW-Authenticate: Bearer error="invalid_token"` | `401` |
| The key set cannot be fetched and no cached key fits the token | `503` with `Retry-After: 5` | `503` |
| Cookie token on a socket whose `Origin` is not allowed (see [chat.md](chat.md)) | n/a | `403` |
| Cookie token from a page not trusted, or a cookie create without `Content-Type: application/json` ([Cookies and other sites](#cookies-and-other-sites)). Checked before the token, so `403` rather than `401` | `403` | n/a |

Every rejection has an empty body (`Content-Length: 0`). A gateway `401` carries the RFC 6750
challenge: exactly `WWW-Authenticate: Bearer` when no token could be read from the request, and
exactly `WWW-Authenticate: Bearer error="invalid_token"` when one was read and failed
verification. Chat's `401` carries no `WWW-Authenticate` header. The gateway's responses carry
`X-Request-Id`; quote it when reporting a problem. Which check failed is not sent to the client.

Client action:

- `401`: get a fresh token from Askedin (sign in again, or refresh) and retry once. A second
  `401` with a fresh token means a configuration mismatch (issuer, audience, key set); report it
  with the request id.
- `503`: retry after the `Retry-After` delay. Do not sign the user out; the token may be fine.

Authentication happens after routing and after the per-route header checks, so an unknown path
answers `404`, a wrong method `405`, and a malformed `Upload-Offset` `400`, whatever the token.

`/api/v1/healthz`, `/api/v1/readyz` and `/metrics` need no token.

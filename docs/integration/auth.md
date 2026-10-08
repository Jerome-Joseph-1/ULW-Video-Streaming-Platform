# Authentication

Every upload and playback endpoint, and the chat WebSocket, needs an access token from the
operator's identity provider. The service keeps no users and issues no tokens of its own
(ADR-0018): it verifies the provider's JWTs in-process against the provider's published key set,
and the user is whatever the token's subject claim says.

## Where the token goes

<!-- infra/auth/src/token_extractor.cpp, infra/auth/include/infra/auth/token_extractor.hpp -->

| Source | Form | Notes |
|---|---|---|
| `Authorization` header | `Bearer <token>` | Scheme matched without regard to case, then one or more spaces. Wins over the cookie when both are present. |
| Cookie | `<cookie name>=<token>` | Name from `ULW_AUTH_COOKIE`, default `auth_token`. The value may be wrapped in double quotes. |

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
- Cookie writes work only same-origin, with the API on the page's own host: a page on
  `example.com` calling `www.example.com`, or the reverse, gets `403` unless
  `ULW_ALLOW_SAME_SITE=1` is set.
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
| Header `alg` | `RS256`, `PS256`, `ES256` or `EdDSA`. Anything else, `none` included, is refused. |
| Header `kid` | Required, 1 to 256 bytes, and must name a key in the key set |
| Header `crit` | Must be absent |
| Algorithm vs key | The key decides. An RSA key (2048 to 8192 bits) verifies `RS256`/`PS256`, an EC P-256 key `ES256`, an OKP Ed25519 key `EdDSA`. A key that publishes its own `alg` accepts only that one. A token whose `alg` does not fit its key is refused. |
| Keys used | Only keys with `use` absent or `sig`, and `key_ops` absent or containing `verify` |
| `iss` | Must equal `JWT_ISSUER` exactly |
| `aud` | A string equal to `JWT_AUDIENCE`, or an array of strings containing it. |
| `exp` | Required, NumericDate. Refused once `now - 60 s >= exp`. |
| `nbf` | Optional. Refused while `now + 60 s < nbf`. |
| Clock skew | 60 s on both `exp` and `nbf` |
| `iat` | Not consulted. Present or not, in the future or not, of any type, it changes nothing; only `exp` and `nbf` bound a token's lifetime. |
| `email` | Optional string, at most 254 bytes, no control characters. `null` counts as absent. |

The signature is checked before any claim is read.

## How the user id is derived

<!-- infra/auth/src/claims.cpp (subject_of), ops/src/dev_only.cpp (token_rules), core/src/ids.cpp (UserId::parse) -->

The user is the claim `ULW_JWT_SUBJECT_CLAIM` names, `sub` unless set:

1. `sub` must be a string (RFC 7519).
2. Another claim, for a provider that puts its user id elsewhere (`user_id`, say, or a
   namespaced `https://example.com/uid`), may be a string or a non-negative integer, taken in
   decimal (`42` becomes `"42"`).
3. The claim absent, or an empty string: refused. No other claim stands in for it; with
   `ULW_JWT_SUBJECT_CLAIM=user_id`, a token's `sub` is not read at all.

The result must be 1 to 128 characters from `A-Z a-z 0-9 . _ : @ | + -`; anything else is
refused rather than cleaned up. This string is the owner of every video the user uploads, and
it is compared byte for byte. Two tokens with different subjects are two different users.

## Service tokens

<!-- infra/auth/src/service_claim.cpp (read_service_claim, claim_holds, names_client), infra/auth/src/claims.cpp (is_service), apps/chat/src/service_api.cpp, apps/gateway/src/connection.cpp (start_service_route), docs/adr/0096-member-lists-changed-by-their-users.md, docs/adr/0097-videos-shared-by-visibility-and-service-grants.md -->

The operator's own backend calls chat's service API ([chat.md](chat.md#the-service-api)) and the
gateway's grants API ([videos-and-playback.md](videos-and-playback.md#service-api-grants)) with a
token of its own,
which it obtains from the same identity provider with the OAuth client-credentials grant. Such
a token must pass every check above (signature, `iss`, `aud`, `exp`, a subject: the client's
id is fine), comes in the `Authorization: Bearer` header (the gateway refuses a service
token in the cookie with `403`), and is the service's when its claim `ULW_SERVICE_CLAIM` (default `scope`) holds
`ULW_SERVICE_SCOPE`, and, when `ULW_SERVICE_CLIENT_ID` is set (chat requires it with its
service port), its `azp` or `client_id` claim names that client:

- a string equal to it, or listing it among values separated by spaces, as OAuth's `scope` does
  (`"openid ulw:admin"`);
- an array with a string that does either (`"roles": ["ulw:admin"]`);
- `true`, when `ULW_SERVICE_SCOPE` is `true`.

Set `ULW_SERVICE_CLIENT_ID` to the backend's client id (the gateway recommends it; chat requires
it with its service port). Choose a scope only the backend's client is ever granted, too: an
identity provider that lets a signed-in user ask for any scope would otherwise let that user act
as the backend, and the client id binding closes that even where scopes are loosely granted. A
service token is an ordinary token everywhere else: it may call every user route as the user its
subject names, and is charged the per-user request limits under that subject. The same settings,
read the same way (`infra/auth/service_claim.hpp`), are what every ULW service that takes service
calls uses.

## Key set

<!-- apps/gateway/src/config.cpp (load_auth), infra/auth/src/jwks_verifier.cpp, infra/auth/include/infra/auth/jwks_verifier.hpp, apps/gateway/src/gateway.cpp and apps/chat/src/chat.cpp (on_signal, drop_auth_caches) -->

| Setting | Meaning |
|---|---|
| `JWKS_URL` | The identity provider's JWK set. Must be `https://`; the process refuses to start otherwise. |
| `JWT_ISSUER` | Required. The provider's `iss`, byte for byte. |
| `JWT_AUDIENCE` | Required with `JWKS_URL`: the `aud` the provider puts in tokens meant for ULW. No default, since a guessed one would refuse every token or accept tokens meant for another service; the process refuses to start without it (exit 2). With `ULW_DEV_JWKS_FILE` it defaults to `ulw-dev`, what `ulw_devtoken` mints. |
| `ULW_JWT_SUBJECT_CLAIM` | Optional, default `sub`: the claim that names the user ([How the user id is derived](#how-the-user-id-is-derived)). 1 to 64 of `A-Z a-z 0-9 _ . : / -`, and not `iss`, `aud`, `exp`, `nbf`, `iat` or `jti` (claims that name no user); anything else stops the process at startup. |
| `ULW_AUTH_COOKIE` | Optional, default `auth_token`. |
| `ULW_SERVICE_CLAIM` | Optional, default `scope`: the claim that marks the operator's backend ([Service tokens](#service-tokens)). 1 to 64 of `A-Z a-z 0-9 _ . : / -`, and not `iss`, `aud`, `exp`, `nbf`, `iat` or `jti`. Without `ULW_SERVICE_SCOPE`, `scope` (or unset) means service calls are off; any other name stops the process at startup. |
| `ULW_SERVICE_SCOPE` | Optional, default none: the value that claim must hold, such as `ulw:admin`. 1 to 128 printable ASCII characters, no spaces. Unset: no token is a service's, and the service API answers `403` to all. |
| `ULW_SERVICE_CLIENT_ID` | Optional, recommended (required by chat with `ULW_SERVICE_PORT`): the backend's client id; only tokens whose `azp` or `client_id` is this are the service's. 1 to 128 printable ASCII characters, no spaces. Without `ULW_SERVICE_SCOPE` it changes nothing: service calls are off. |
| `ULW_JWKS_MAX_STALE_HOURS` | Optional, 1 to 168, default 24: how long keys stay trusted while every refetch fails (below). |
| `ULW_DEV_JWKS_FILE` | Development only: a local Ed25519 key set instead of `JWKS_URL`. Setting both is a startup error. It is refused (exit 2) unless `ULW_DEV_MODE=1`, and refused regardless inside a Kubernetes pod (`KUBERNETES_SERVICE_HOST` set, as the kubelet does in every container), so a key set left in a real deployment's configuration stops the process instead of being trusted. The gateway and chat server both apply this. The first log line prints `keys=DEVELOPMENT <file>` so it cannot go unnoticed. |
| `ULW_DEV_MODE` | `0` or `1`, default `0`. `1` says this is a development run, which development-only settings such as `ULW_DEV_JWKS_FILE` need. |

Caching and refresh:

- The key set is fetched in the background and refetched every 15 minutes. A key withdrawn from
  the set keeps verifying until the next successful refetch.
- Keys are trusted for at most `ULW_JWKS_MAX_STALE_HOURS` (1 to 168, default 24) after the last
  successful fetch. Past that, while every refetch fails, the keys and every cached verdict are
  dropped and each token is refused as if the key set were unavailable, until a fetch succeeds:
  a key the provider withdraws while its JWKS cannot be reached stops verifying within a day. The
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
- SIGHUP requests a drop of the auth caches (gateway and chat; ADR-0082): a fetch starts at
  once, replacing one in flight, with the retry backoff counted from 1 s again. Refetch first,
  then swap: while it runs, the cached keys and remembered tokens go on answering as before, and
  when it succeeds it replaces the key set and forgets every remembered verified token and
  unknown `kid` in one step, so a token whose key left the set is refused with `401` from then
  on. Each request logs `auth cache drop requested; completes on the next successful key fetch`
  at info level and counts in `auth_cache_drops_total`; the `auth_cache_drop_pending` gauge
  reads 1 from the SIGHUP until a fetch completes the drop. If the key set cannot be fetched,
  nothing is dropped: the cached keys, the old key's included, keep working until a fetch
  succeeds, and the drop stays pending through the retries. Chat sockets already open are not
  closed; they run to their token's `exp`.

So a newly rotated-in key is accepted within one fetch of first use, provided the provider
publishes it before issuing tokens with it, and a withdrawn key stops verifying once the fetch after a
SIGHUP succeeds.

## Configuring an identity provider

<!-- deploy/kubernetes/overlays/*/config.env, tests/unit/auth/jwks_verifier_test.cpp (provider_token) -->

Any provider that signs JWTs with one of the algorithms above and publishes its keys as a JWK
set works. The operator sets, per environment (in Kubernetes, `JWKS_URL`, `JWT_ISSUER`,
`JWT_AUDIENCE`, `JWT_SUBJECT_CLAIM`, `AUTH_COOKIE` and `ALLOWED_ORIGINS` in the overlay's
`config.env`, deploy/kubernetes/RUNBOOK.md):

| Setting | What to put there |
|---|---|
| `JWKS_URL` | The provider's JWK set over `https://`, as its `jwks_uri` names it |
| `JWT_ISSUER` | Exactly the `iss` its tokens carry: same scheme, host, path and trailing slash or none |
| `JWT_AUDIENCE` | An audience the provider issues for ULW alone, present in every access token meant for it |
| `ULW_JWT_SUBJECT_CLAIM` | `sub`, unless the provider names users in another claim |
| `ULW_AUTH_COOKIE` | The cookie the web app keeps the token in, if it uses one |
| `ULW_ALLOWED_ORIGINS` | The web app's pages that use the cookie ([Cookies and other sites](#cookies-and-other-sites)) |

`iss` is compared byte for byte, so a wrong `JWT_ISSUER` refuses every token with `401`. Read it
from a real token (`echo <token> | cut -d. -f2 | base64 -d`, padding aside) rather than from the
provider's documentation.

Do not use:

- A URL that answers anything but a key set (a web page's `/.well-known/jwks.json` that serves
  HTML, say): every fetch fails and no token ever verifies.
- An in-cluster `http://` URL: `JWKS_URL` must be `https://`, and the process refuses to start
  otherwise.
- OIDC discovery: ULW never reads `/.well-known/openid-configuration`; it needs `JWKS_URL` and
  `JWT_ISSUER` set explicitly.

The key set is fetched as `{"keys":[...]}`; ULW ignores its caching headers and refetches every
15 minutes as above. `typ`, `jti`, `iat` and claims other than `iss`, `aud`, `exp`, `nbf`, the
subject claim and `email` are not read. Nothing may use `jti` as a token id, a replay key or a
cache key here: some providers set it to the `kid`, the same for every token under a key, and the
verdict cache keys on the token's SHA-256.

### Other tokens under the same key

A provider may sign tokens that must never authenticate at ULW (a two-factor challenge, a
personal-access-token swap) with the same key as its access tokens. Only the audience tells them
apart, so `JWT_AUDIENCE` must be one that only access tokens for ULW carry; never set it to the
audience of such a token. A test (`JwksVerifierTest.TwoFactorAndPatTokensAreRefusedForTheirAudience`)
keeps tokens under another audience refused with `401` (`WrongAudience`).

### Key rotation

A provider that rotates with no overlap (the old `kid` leaves the JWKS the moment the new key is
made) leaves ULW, left alone, accepting tokens signed with the old key for up to 15 minutes, from
the cached key set and the remembered verified tokens (Caching and refresh, above).

So the operator's rotation procedure must also reach ULW once the rotation has committed, in the
rotated environment: SIGHUP every `gateway_server` and `chat_server` process
(deploy/kubernetes/RUNBOOK.md, "8. Signing key rotation"), or restart them. Afterwards each pod's
`auth_cache_drops_total` has gone up by one and its `auth_cache_drop_pending` is back to 0, and a
token under the old key gets `401`.

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

- `401`: get a fresh token from the identity provider (sign in again, or refresh) and retry once. A second
  `401` with a fresh token means a configuration mismatch (issuer, audience, key set); report it
  with the request id.
- `503`: retry after the `Retry-After` delay. Do not sign the user out; the token may be fine.

Authentication happens after routing and after the per-route header checks, so an unknown path
answers `404`, a wrong method `405`, and a malformed `Upload-Offset` `400`, whatever the token.

`/api/v1/healthz`, `/api/v1/readyz` and `/metrics` need no token.

Chat's service API ([chat.md](chat.md#the-service-api), on chat's own `ULW_SERVICE_PORT`) takes
service tokens as [Service tokens](#service-tokens) describes, read from the same two settings,
and only from `Authorization: Bearer`, never the cookie. It answers `401` (with
`WWW-Authenticate: Bearer`) for a missing or failing token, `403` for a valid token that is not
the service's, and `503` when the key set cannot be fetched, each with a JSON body naming the
reason. Setting up the backend's client in an identity provider, step by step:
deploy/kubernetes/RUNBOOK.md, step 10.

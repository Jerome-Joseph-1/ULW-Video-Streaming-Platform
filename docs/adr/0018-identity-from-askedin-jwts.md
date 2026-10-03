# 0018. Identity is borrowed from Askedin

Status: Accepted, amended by 0088 (identity is the operator's provider: no default audience against a JWKS, a configurable subject claim, no `id` fallback)
Date: 2026-09-28

## Context

ULW runs inside Askedin's platform, and its users already have Askedin accounts and sessions.
Askedin issues JWTs signed with keys it publishes as a JWKS. Browsers present them in the
`auth_token` cookie (`auth_token_stage` on staging) or as `Authorization: Bearer`. An earlier ULW
design issued its own tokens, signed with a pinned Ed25519 key.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Self-issued tokens signed with a pinned Ed25519 key (the earlier design) | No runtime dependency on Askedin; one key; fast verification | Rejected: a second identity beside Askedin's, with its own login and account linking |
| Trust `x-user-*` headers set at the edge | Verification happens once; services read a header | Rejected: any in-cluster caller or misrouted request can set them, and a service cannot tell who wrote the header |
| A local users table synchronized from Askedin | Foreign keys and joins on users | Rejected: a copy of data Askedin owns, kept current by a sync that can drift |
| Verify Askedin JWTs in-process against Askedin's JWKS | One identity; nothing trusted from headers; no network call per request once keys are cached | Accepted |

## Decision

- Services verify Askedin-issued JWTs in-process, taken from the `auth_token` or
  `auth_token_stage` cookie or `Authorization: Bearer`, against Askedin's JWKS. Configuration is
  `JWKS_URL`, `JWT_ISSUER` and `JWT_AUDIENCE` (default `askedin-platform`).
- The algorithm is taken from the key, never from the token's header, so a token cannot choose a
  weaker check (`alg: none`, or HMAC keyed with the public key).
- Identity comes from the `sub` claim, falling back to `id`, plus `email`. There is no local users
  table: `owner_id` is the JWT `sub` as text (ADR-0023).
- Inbound `x-user-*` headers are never trusted.
- A local Ed25519 verifier behind the same `IJwtVerifier` port serves offline development.

## Consequences

- Services need Askedin's JWKS for new keys. With keys cached, a JWKS outage only matters if
  Askedin rotates keys during it.
- A stolen token stays valid until it expires; ULW keeps no revocation list.
- A user is only a string. Names and profile data come from Askedin or the token's claims.
- A change to claim names, issuer or audience at Askedin breaks sign-in here. Monitor verification
  failures by reason: expired, bad signature, wrong issuer or audience, unknown key id.
- Reopen if ULW must serve users who have no Askedin account.

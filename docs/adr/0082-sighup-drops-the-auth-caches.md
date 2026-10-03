# 0082. SIGHUP drops the auth caches

Status: Accepted
Date: 2026-10-02

## Context

The gateway and chat_server verify Askedin's tokens against a JWK set they cache (ADR-0018,
`infra/auth/include/infra/auth/jwks_verifier.hpp`). The keys are refetched every 15 minutes and
a verified token is remembered by its digest for up to 15 minutes, never past its `exp`. A key
withdrawn from the set therefore keeps verifying here until the next refetch, and so does every
verdict it gave.

Askedin's auth-service rotates its signing key without overlap: one transaction creates the new
key and deactivates the old one, and the old kid leaves the JWKS at once (the owner's document
dated 2026-10-03; docs/integration/auth.md, Askedin). Its rotation runbook restarts the services
that verify its tokens, so that none goes on accepting the old key. Until now ULW had no such
hook: tokens signed with the old key went on authenticating at ULW for up to 15 minutes after a
rotation. A token under the new key arriving at least 10 s after the last fetch does trigger a
fetch that drops the old key and its verdicts, but nothing guarantees one arrives, and an
operator cannot tell when it has.

Both processes already watch SIGHUP on the reactor (`net/signals.hpp`). The gateway rereads its
TLS certificate on it; chat_server ignored it.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Restart the processes in Askedin's rotation runbook | Needs no code, and is what Askedin does for its own services | Kept as the fallback, not the answer: a rolling restart of two replicas takes a minute or more while old pods serve, drains every upload and chat socket, and depends on Askedin's runbook knowing ULW's deployments |
| Shorten the key and verdict lifetimes | Narrows the window for every rotation without anyone acting | Rejected: a window of any length remains, and every process fetches proportionally more often for the rare rotation |
| Honour the JWKS `Cache-Control` | Askedin says how long its set may be cached | Rejected: it says an hour, longer than ULW's 15 minutes, and a header does not announce a withdrawal |
| An admin HTTP endpoint that drops the caches | Callable from outside the pod | Rejected: one more authenticated surface on a public listener, for an action an operator with cluster access can already take |
| SIGHUP empties the key set and every remembered verdict at once, then fetches | Nothing is answered from the old keys after the signal | Rejected: a JWKS outage at that moment leaves no keys at all, and every token is answered `503` until a fetch succeeds; a routine rotation, or a SIGHUP sent for a certificate, should not be able to take sign-in down |
| SIGHUP fetches at once, and the fetch that succeeds replaces the keys and forgets every remembered verdict and unknown kid (refetch first, then swap) | One signal per pod, already the reload signal, keeps every connection, and an outage changes nothing until it ends | Accepted |

## Decision

- `IJwtVerifier` gains `drop_caches()` and `drop_pending()`. `JwksVerifier`'s `drop_caches()`
  requests the drop: it marks it pending, counts the retry backoff from its first step again,
  cancels the fetch in flight (its answer may predate the rotation) and starts another. The keys,
  verdicts and remembered unknown kids in hand are left alone, and keep answering.
- The next fetch that succeeds completes the drop: it forgets every remembered verdict and
  unknown kid, then installs the new key set as any fetch does (so the kids sought during it and
  missing from it are remembered afresh), and clears the pending mark. A failed fetch keeps the
  keys and leaves the drop pending for the retries. A fixed key set (`Ed25519LocalVerifier`) has
  nothing to drop and is never pending.
- The gateway and chat_server call it on SIGHUP, on the reactor thread like everything else the
  verifier does. The fetch is the libcurl multi's, as every other key fetch is, so nothing
  blocks the loop. The gateway still rereads its certificate on the same signal.
- Each request logs `auth cache drop requested; completes on the next successful key fetch` at
  info level and counts in `auth_cache_drops_total`; the `auth_cache_drop_pending` gauge reads 1
  until a fetch completes it (gateway and chat `/metrics`), so the operator who sent the signal
  can see it landed, and finished, on every pod.
- Askedin's rotation runbook is asked to SIGHUP (or restart) every gateway and chat pod in the
  environment once the rotation commits (deploy/askedin/RUNBOOK.md, 8).

## Consequences

- After a SIGHUP a token signed with a withdrawn key is answered from the cache while the fetch
  runs, about one round trip, and refused (`401`) once it lands:
  `JwksVerifierTest.DroppingTheCachesRefusesATokenWhoseKeyLeftTheSet`. Without a SIGHUP it goes
  on verifying until the next refetch:
  `JwksVerifierTest.AWithdrawnKeyGoesOnVerifyingUntilTheNextRefetchWithoutADrop`.
- A SIGHUP while Askedin's JWKS cannot be reached drops nothing yet: tokens under the old key
  keep working until a retry succeeds, from 1 s and then with the usual backoff to a minute, and
  `auth_cache_drop_pending` stays at 1 meanwhile
  (`JwksVerifierTest.ADropWhoseFetchFailsKeepsTheKeysUntilARetryCompletesIt`). Sign-in never
  depends on the JWKS being up at the moment of a SIGHUP. For a suspected key compromise during
  an outage the operator restarts the pods instead, which fails closed: new pods have no keys
  until a fetch succeeds. The key expiry (`ULW_JWKS_MAX_STALE_HOURS`) still bounds how long any
  key outlives a long outage.
- A completed drop costs every token one signature check again; no connection is closed.
- Chat sockets already open are not closed by a drop: they run to their token's `exp`, at most
  an hour for Askedin's tokens, as ADR-0073 has them. A token refused after the drop opens no new
  socket.
- Running the signal on every pod is the operator's job; a pod that missed it is visible as an
  `auth_cache_drops_total` that did not move, and one whose drop has not completed as an
  `auth_cache_drop_pending` of 1.

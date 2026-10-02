# 0079. SIGHUP drops the auth caches

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
| SIGHUP drops the key set and every remembered verdict, and starts a fetch at once | One signal per pod, already the reload signal, immediate, keeps every connection | Accepted |

## Decision

- `IJwtVerifier` gains `drop_caches()`. `JwksVerifier`'s forgets its keys, every remembered
  verdict and every remembered unknown kid, cancels the fetch in flight (its answer may predate
  the rotation) and starts another. Requests that arrive meanwhile wait on that fetch as on an
  unseen key, so none is answered from what was dropped. A fixed key set
  (`Ed25519LocalVerifier`) has nothing to drop.
- The gateway and chat_server call it on SIGHUP, on the reactor thread like everything else the
  verifier does. The fetch is the libcurl multi's, as every other key fetch is, so nothing
  blocks the loop. The gateway still rereads its certificate on the same signal.
- Each drop logs `auth caches dropped` at info level and counts in `auth_cache_drops_total`
  (gateway and chat `/metrics`), so the operator who sent the signal can see it landed on every
  pod.
- Askedin's rotation runbook is asked to SIGHUP (or, failing that, restart) every gateway and
  chat pod in the environment once the rotation commits (deploy/askedin/RUNBOOK.md).

## Consequences

- After a SIGHUP a token signed with a withdrawn key is refused (`401`) as soon as the fetch
  lands, about one round trip: `JwksVerifierTest.DroppingTheCachesRefusesATokenWhoseKeyLeftTheSet`.
  Without it the token goes on verifying until the next refetch:
  `JwksVerifierTest.AWithdrawnKeyGoesOnVerifyingUntilTheNextRefetchWithoutADrop`.
- A SIGHUP while Askedin's JWKS cannot be reached leaves nothing to verify with: every token is
  answered `503` (`KeysUnavailable`) until a retry succeeds, 1 s after the failure and then
  with the usual backoff to a minute. This is the fail-closed choice the key expiry
  (`ULW_JWKS_MAX_STALE_HOURS`) already makes; a SIGHUP sent only to reload a certificate costs
  one key fetch and carries the same risk, so it should not be sent while the JWKS is down.
- A drop costs every token one signature check again, and every cached verdict refilled; no
  connection is closed.
- Chat sockets already open are not closed by a drop: they run to their token's `exp`, at most
  an hour for Askedin's tokens, as ADR-0073 has them. A token refused after the drop opens no new
  socket.
- Running the signal on every pod is the operator's job; a pod that missed it is visible as an
  `auth_cache_drops_total` that did not move.

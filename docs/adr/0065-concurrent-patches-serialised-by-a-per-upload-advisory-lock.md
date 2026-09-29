# 0065. Concurrent PATCHes are serialised by a per-upload advisory lock

Status: Accepted
Date: 2026-09-29

## Context

Two PATCHes to the same upload at once would both append at the same offset, or one at an offset
the other is about to pass. The store's parts are ordered and immutable once complete, so the
result would be a corrupt object or a lost chunk. ADR-0009 fixes the protocol's answer: a
request that does not match the upload's state gets 409 with the current `Upload-Offset`.
A precondition such as "the offset must match" is not enough on its own: two requests can both
pass it before either writes. Something has to be held while a chunk is being appended, and it
has to disappear when the gateway that holds it does.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Check the offset in the request only | No lock | Rejected: two requests can both read the same offset and both proceed |
| `pg_try_advisory_xact_lock(hashtext(upload_id))` in a transaction wrapping the chunk | The form the brief names | Rejected for the append: a transaction lasts as long as the chunk streams, which is minutes at a slow client's rate and holds a pooled connection throughout, and `hashtext` is 32 bits, so distinct uploads would share a key |
| A row lock (`SELECT ... FOR UPDATE`) held for the chunk | Standard | Rejected: same open-transaction problem, and a second request would wait instead of being refused |
| A session-level advisory lock on a key derived from the upload id, held on one dedicated connection for the life of the claim | Held across the whole stream without a transaction; the server drops it when the session ends, so a killed gateway frees its uploads at once; `try` never waits | Accepted |

## Decision

- Before a PATCH appends, the gateway claims the upload (`IUploadCatalog::claim_upload`).
  `PgUploadCatalog` claims with `pg_try_advisory_lock($1)` on a dedicated session, restricted to
  a row of the upload owned by the caller, so an upload the caller does not own is `NotFound`
  and locks nothing (`infra/postgres/src/upload_catalog.cpp`).
- The key is 63 bits from the random parts of the UUIDv7 (`infra/postgres/src/lock_key.hpp`),
  because the first 48 bits are a timestamp and would collide within a millisecond. Two live
  uploads share a key with probability 2^-63; sharing only refuses claims on one while the
  other is held.
- A held lock is refused, not waited on: the claim returns `Conflict`, and the gateway answers
  409 with the upload's current durable offset, read from the catalog, so the client can resume
  from it (`Connection::on_claimed`, `apps/gateway/src/connection.cpp`). The same 409 with the
  offset answers a PATCH whose offset does not match, and one to an upload that is no longer
  active.
- Advisory locks are reentrant within a session, so the catalog also keeps a map of its own
  claims and refuses a second claim of the same upload from the same process.
- The claim is released when the request ends (`release_claim`). It is a claim on appends. A
  commit does not take it (ADR-0049); the reaper takes the transaction-level form of the same
  key and skips an upload whose claim is held.
- All claims of one process live on one connection. If that connection is lost every claim goes
  with it: claims in flight fail `Unavailable`, and `record_progress` under a lost claim fails
  `Conflict`. The holder claims again.

## Consequences

- One appender per upload, across every gateway replica, with no waiting and no shared
  in-process state; a crashed gateway frees its uploads when its session ends.
- The lock costs a dedicated connection, held for the life of the process; it does not come out
  of the request pool.
- Divergence from the brief, plainly: the brief says
  `pg_try_advisory_xact_lock(hashtext(upload_id))` "or equivalently a per-upload lock held for
  the chunk". The code takes the second form: `pg_try_advisory_lock` (session level) on a 63-bit
  key, not `hashtext`. Behaviour the brief asked for holds: a held lock gives 409 with the
  current `Upload-Offset` (`tests/gateway/gateway_upload_test.cpp`,
  `ConcurrentAppendToOneUploadIs409`; `postgres_catalog_test.cpp`,
  `ClaimReturnsTheUploadAndRefusesASecondGateway`).

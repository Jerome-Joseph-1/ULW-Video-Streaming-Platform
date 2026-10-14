# 0108. A create's Idempotency-Key names one upload per owner, kept in its own table

Status: Accepted
Date: 2026-10-13

## Context

`POST /api/v1/uploads` mints new ids for every request. A client whose create timed out, or whose
answer was lost, cannot tell whether the video was made; retrying makes a second one, which sits
in `init` in the owner's list. ADR-0106 lets the client delete it, but it has to notice first.
Askedin asked for a way to make the retry safe.

What had to hold: a retry answers the first create's ids and makes nothing; the same key for a
different file is an error, not a silent answer about another file; two concurrent retries make
one video; the key costs nothing to a client that sends none; and the migration locks and scans
nothing a running gateway needs.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A column on `uploads` with a unique index on `(owner_id, key)` | One table | The index build scans `uploads` holding its writes for the migration | Rejected |
| A table `upload_requests (owner_id, key) → upload_id`, with a foreign key to `uploads` that cascades | Nothing scanned; the row goes when the purge deletes the upload | The foreign key's lock on `uploads` for the migration's transaction (writes wait, briefly; the table is empty) | Accepted |
| Keys that expire after a day | Bounded table | A reaper pass for them; a client holding a key longer gets a second video | Rejected: a key lives as long as its upload, which the purge already bounds |
| Derive the ids from the key | No table | A key reused for another file would collide on the video id, and ids would no longer be UUIDv7 in time order | Rejected |

## Decision

- The header is optional, 1 to 255 visible ASCII characters, checked at the request head (`400`
  otherwise) and by the table's check.
- The gateway looks the key up first (`find_keyed_upload`). A key that names an upload answers
  `201` with that upload's ids and chunk size when the request's fingerprint (size, parsed content
  type, filename) matches, and `409` `idempotency_key_reused` when it does not. Only a key nobody
  holds starts the store's session.
- The insert takes the key in the same statement as the video and the upload
  (`INSERT ... ON CONFLICT DO NOTHING`), and makes them only if it took the key. A concurrent
  create that lost the key finds nothing made, discards its store session, and answers the
  winner's upload. The statement retried after its answer was lost finds the key naming its own
  upload and succeeds.
- Migration 0021 creates the table, its check and an index on `upload_id` for the cascade.

## Consequences

- Main only (v1.1 and later): the maintenance lines' migrations stop at 0017 and 0018.
- A retry answers `durable_offset` 0, as a create does; the client asks `HEAD` for the offset.
- A key reused after its video was purged starts a new upload.

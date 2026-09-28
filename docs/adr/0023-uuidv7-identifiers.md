# 0023. Identifiers are UUIDv7

Status: Accepted
Date: 2026-09-28

## Context

Videos, uploads, rooms and messages need ids that services mint without coordinating, that index
well in Postgres, and that appear in URLs, storage keys and cache keys. Users are not ours: their
ids come from Askedin's tokens (ADR-0018).

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| UUIDv4 | Universal; native Postgres `uuid` | Rejected: random ids scatter inserts across the B-tree index |
| ULID | Time-ordered, and 26 characters instead of 36 | Rejected: no native Postgres type, and a second textual format beside the UUIDs every library already speaks |
| `bigserial` everywhere | Compact and ordered | Rejected: the database has to mint the id before a storage key can exist, and sequential public ids are guessable and reveal volume |
| UUIDv7 (RFC 9562) as Postgres `uuid` | Time-ordered, so inserts append to the index; native type; standard text form | Accepted |

## Decision

- Entity ids are UUIDv7 (RFC 9562), stored as Postgres `uuid`. On the wire they are the canonical
  lowercase 36-character form: 32 hex digits in 8-4-4-4-12 groups.
- Input ids are validated exactly and rejected otherwise, never sanitized: no uppercase, braces or
  missing dashes. A second accepted spelling would give one entity two names in URLs, storage keys
  and cache keys.
- `jobs.id` is an internal `bigserial`; it never leaves the database and the worker.
- User ids are not ours. `owner_id` is `text` holding the JWT `sub`, bounded to 1..128 bytes,
  checked against an allowed character set, with no foreign key. 128 bytes bounds an untrusted
  claim with room to spare over a 36-character UUID, without depending on Askedin's exact format.

## Consequences

- A UUIDv7 carries its creation time to the millisecond; anyone holding an id learns when the
  entity was created.
- Ids minted in the same millisecond are unordered among themselves. Nothing may rely on id order
  below a millisecond; message order in a room comes from its owner (ADR-0015).
- With no foreign key on `owner_id`, a user deleted at Askedin leaves rows behind until a
  separate deletion path removes them.
- Reopen if ids must be shorter in URLs. A shorter encoding of the same 128 bits is a
  presentation change, not a new id scheme.

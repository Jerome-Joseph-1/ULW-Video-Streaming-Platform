# 0064. The first accepted PATCH moves a video from `init` to `uploading`

Status: Accepted
Date: 2026-09-29

## Context

A video is created in `init` when the client creates an upload. Until something happens on the
wire, nothing is uploading: the client may never send a byte. Playback, the owner's listing and
the reaper (ADR-0049) all read the state, and `uploading` should mean that bytes have been
accepted. The domain model owns the state machine (`core/`), and the brief names the method
that makes the transition, `Video::start_upload()`.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| `uploading` at creation | No transition to write | Rejected: a video whose client never sends anything would read as in progress |
| A separate call or event from the client | Explicit | Rejected: a second request to be forgotten, and the state would lie between it and the first byte |
| On the first PATCH the store accepts, in the write that records the progress | The state follows the bytes; the same write cannot half-succeed | Accepted |

## Decision

- The transition is `init -> uploading` and happens when a PATCH's first chunk is durable and its
  progress is recorded, not when the request arrives or is authorised. A PATCH that is refused
  (conflict, wrong offset, bad token) leaves the video in `init`.
- `Video::start_upload()` (`core/src/video.cpp`) is the domain rule: allowed from `Init` only,
  otherwise a `DomainError`. `start_processing` accepts both `Init` and `Uploading`, so a small
  upload that commits without a recorded progress write still reaches `processing`.
- The catalog applies the transition in the statement that records progress. In Postgres it is
  the `started` CTE of `kRecordProgress` in `infra/postgres/src/upload_catalog.cpp`:
  `UPDATE videos SET state = 'uploading', version = version + 1 ... WHERE state = 'init'` for the
  upload's own video, in one statement with the `durable_offset` update. The in-memory catalog
  does the same in `record_progress` (`infra/catalog/memory/src/memory_catalog.cpp`). Later
  PATCHes match no `init` row and change nothing, so they cost no version bump.

## Consequences

- The video and the upload's offset cannot disagree: both change in one statement.
- Divergence from the brief, recorded plainly: production code does not call
  `Video::start_upload()`. Only its unit tests (`tests/unit/core/video_test.cpp`) do. The
  gateway never loads a `Video` aggregate to transition it; the catalog adapters restate the
  rule in SQL (`WHERE state = 'init'`) and in the memory catalog. The domain method and the SQL
  therefore agree by convention, and `tests/integration/postgres_catalog_test.cpp` (progress
  moves an `init` video to `uploading`) is what keeps them together. Either the gateway should
  route the transition through the aggregate, or the method should be removed as unused; that is
  a code change and not made here.
- `version` increments once per transition, so a client polling by version sees exactly one
  change when bytes first land.

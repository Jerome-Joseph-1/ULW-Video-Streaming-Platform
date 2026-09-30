# 0064. The first accepted PATCH moves a video from `init` to `uploading`

Status: Accepted
Date: 2026-09-29

## Context

A video is created in `init` when the client creates an upload. Until something happens on the
wire, nothing is uploading: the client may never send a byte. Playback, the owner's listing and
the reaper (ADR-0049) all read the state, and `uploading` should mean that bytes have been
accepted. The brief names a domain method for the transition, `Video::start_upload()`.

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
- The catalog is the single authority for this transition; the domain model has no method for
  it. `Video::start_processing` accepts both `Init` and `Uploading`, so a small upload that
  commits without a recorded progress write still reaches `processing`.
- The catalog applies the transition in the statement that records progress. In Postgres it is
  the `started` CTE of `kRecordProgress` in `infra/postgres/src/upload_catalog.cpp`:
  `UPDATE videos SET state = 'uploading', version = version + 1 ... WHERE state = 'init'` for the
  upload's own video, in one statement with the `durable_offset` update. The in-memory catalog
  does the same in `record_progress` (`infra/catalog/memory/src/memory_catalog.cpp`). Later
  PATCHes match no `init` row and change nothing, so they cost no version bump.

## Consequences

- The video and the upload's offset cannot disagree: both change in one statement.
- Divergence from the brief, recorded plainly: there is no `Video::start_upload()`. The gateway
  never loads a `Video` aggregate to transition it, so a domain method would only restate the
  catalog's rule and could drift from it; it was removed as unused. Both catalogs are pinned by
  tests: `FirstProgressStartsTheVideoAndOffsetsNeverMoveBack`
  (`tests/integration/postgres_catalog_test.cpp`) and
  `GatewayUpload.OnlyTheFirstAcceptedPatchMovesTheVideoFromInitToUploading`
  (`tests/gateway/gateway_upload_test.cpp`, the memory catalog).
- `version` increments once per transition, so a client polling by version sees exactly one
  change when bytes first land.

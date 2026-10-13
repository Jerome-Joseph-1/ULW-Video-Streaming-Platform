# 0106. Deleting a video mid-upload aborts its upload, and cancelling an upload deletes its video

Status: Accepted
Date: 2026-10-13

## Context

ADR-0100 let a video's owner, and the operator's backend, delete a committed video, and refused
one whose upload was still open (`409` `not_committed`): the owner had the upload's cancel for
that. The product using the platform (Askedin) found three gaps:

- **The backend cannot clean up an upload a user abandoned.** Its service identity is not the
  upload's owner, so `DELETE /api/v1/uploads/{id}` is not open to it, and the takedown refused
  the video. A user whose account is closed mid-upload left a video the backend could not remove.
- **A cancelled upload left its video behind.** The cancel aborted the upload but kept the video
  in `init` or `uploading` for good: it stayed in the owner's list (`GET /api/v1/videos`, ADR-0100)
  and could not be deleted by anyone.
- **One call for "this video is gone".** A client deleting from its list should not have to know
  whether the upload finished.

What had to hold: an upload aborted this way never completes later (a `PATCH` or commit racing
the delete must not bring the video back); the store's staged parts go with the video; the call
stays idempotent; and no migration, so that the fix ships on the maintenance lines (v0.2.x,
v1.0.x) whose migrations end at 0017 and 0018.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Open `DELETE /api/v1/uploads/{id}` to the service identity | Small | The backend knows videos, not upload ids; the cancelled upload's video still stays behind | Rejected |
| Delete abort the upload and soft-delete the video in one statement, and cancel delete the video the same way | One call for every state; the abort and the delete commit together, so no commit can land between them | The delete's statement now locks the upload's row too | Accepted |
| Hard-delete the rows at once | Nothing left for the reaper | The store's staged parts and any object a racing commit finished need the reaper anyway; ADR-0100's purge already does the rest | Rejected |
| The reaper's orphan sweep (`reap_abandoned`, a week) removes the staged parts | Nothing new | A week of storage per abandoned upload, and the object a commit finished just ahead of the abort is not a session | Rejected: the purge discards the sessions itself |

## Decision

- **Delete** (`kDeleteVideo`, owner and service alike) accepts every state. In one statement it
  aborts the video's open upload (`uploads.state = 'aborted'`), then soft-deletes the video,
  revokes its grants, cancels a queued job and queues the purge, as before. The upload's row is
  locked before the video's, the order a commit (`kCompleteUpload`, then `kStartProcessing`) and
  a progress update take them, so the two never deadlock. A commit that took the upload's lock
  first wins: the delete then finds a committed video and deletes it as before; the job the commit
  queued after the delete's snapshot is cancelled by the reaper's `kDuePurges`. A commit that
  comes second finds the upload aborted and answers `409`. The `409` `not_committed` answer is
  gone.
- **Cancel** (`kAbortUpload`) aborts the upload and, in the same statement, deletes its video if
  it is still `init` or `uploading`, revoking grants and queuing the purge. A repeat, or a cancel
  of an upload aborted earlier (by a cancel before this ADR, or by the reaper's expiry), deletes a
  video still left behind. A completed upload is still `409`.
- **Purge** (`IVideoPurges::due`) hands the reaper, with each due video, the ingest sessions of
  its uploads that never completed. The reaper releases each as it releases an expired upload
  (discard the session, remove the object at its key, confirm the store holds neither) before it
  lists and removes `videos/<id>/`, so a commit still in flight in the store cannot leave an object
  behind the listing. A session it cannot confirm gone keeps the video queued for the next pass.
- **Append and commit after.** The upload's row stays aborted until the purge removes it, so a
  `PATCH` or commit answers `409` (`410` past the upload's expiry) and then `404`. Nothing moves
  an aborted upload back to `active`, and `kCompleteUpload` requires `active`, so the video never
  comes back.

## Consequences

- Clients: `DELETE /api/v1/videos/{id}` and `DELETE /api/v1/service/videos/{id}` no longer answer
  `409` `not_committed`; a cancelled upload's video leaves the owner's list. Both are fixes of
  behaviour the documentation did not promise to keep (versioning rule 3).
- Videos left in `init` by a cancel before this change can be removed by repeating the cancel, by
  the owner's delete, or by the backend's takedown.
- No migration: the change is in statements and the reaper only, and ships on v0.2.1 and v1.0.1.
- Amends ADR-0100's rejection of "delete an upload in progress too".

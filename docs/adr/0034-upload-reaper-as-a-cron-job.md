# 0034. Abandoned uploads are reaped by a CronJob

Status: Accepted
Date: 2026-09-29

## Context

A client that starts an upload and never finishes leaves three things behind: an `active` row
in `uploads` past its `expires_at` (the gateway allows 6 days), a video stuck in `init` or
`uploading`, and an open multipart upload in the bucket, which R2 bills as stored bytes. The
gateway cannot clean up after a client that is not there. `CreateMultipartUpload` is not
idempotent, so a crash between creating the session and recording its id in the catalog leaves
a session no row names.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| In the gateway | The catalog and the store are already there | Rejected: the sweep blocks on the network and the database, the gateway's reactor must never, and two replicas would both run it |
| In the worker's loop, beside the job reaper | One more call in a loop that already reaps leases | Rejected: the worker holds no ingest store, only the transfer port, and its start-up and shutdown are built around one job at a time |
| Only the bucket's lifecycle rule | Nothing to run | Rejected: it frees the bytes after 7 days but leaves the rows and the videos, and a client would be told nothing for a week |
| A separate one-shot binary run by a CronJob | Runs to completion, exits, is supervised by the cluster's scheduler | Accepted |

## Decision

- `ulw_reaper` makes one pass and exits: non-zero if any part failed. It ships in the gateway
  image, which already carries `ulw_migrate`, and reads the gateway's environment
  (`ULW_DATABASE_URL`, storage location, `ULW_BUCKET`, the S3 keys). The RUNBOOK gives the
  CronJob (every 15 minutes, `concurrencyPolicy: Forbid`).
- A pass has two phases. First, `IUploadExpiry::expire` (`PgUploadReaper`) reads the active
  uploads whose `expires_at` has passed, oldest first, in batches of 100. For each, one
  statement sets `uploads.state = 'aborted'`, fails its video (`state = 'failed'`,
  `error_reason = 'upload expired'`, in the same statement, as the failed-job path does) and
  takes `pg_try_advisory_xact_lock` on the key the gateway's claim uses (the fenced pattern of the job
  queue, applied to uploads). Then the store's session is discarded and the object at its key
  removed (not found is the usual answer), and the reaper asks the store for the upload's offset:
  only "not found" counts the upload as expired. Anything else is a release failure, counted
  separately and logged with the key, and the pass exits non-zero. It is not retried, the row
  being aborted already: a session is left for the sweep below, an object that would not go
  stays until someone reads the log.
- The lock protects appends, not commits. A PATCH streaming into an upload holds the session-level
  advisory lock on that key, so the reaper's transaction-level lock fails and the upload is
  skipped until the next pass. A commit does not take the claim: the gateway completes the
  store's session, and only then asks the catalog to complete the upload. Against the catalog the
  race is safe either way: the commit takes the row lock first, and the reaper's update re-tests
  `state = 'active'` on the new row version and touches nothing; or second, and the commit finds
  the upload `aborted` and answers Conflict, never a job for a video that has failed.
- Against the store it is not, which is why the reaper removes the object at the upload's key
  after releasing the session. A commit that finished in the store just before the abort leaves
  a whole object no video references, and neither the sweep (it lists sessions) nor the lifecycle
  rule (it aborts sessions) would ever touch it. Releasing the session first means a commit still
  in flight can no longer complete one; the key is removed second. Safe because the row is
  aborted and its video failed, so nothing can name that key.
- The state is `aborted`, not a new `expired`: the schema's check constraint, the domain model
  and every reader already treat `aborted` as terminal, and the video's `error_reason` says why.
- The store is released after the row is aborted, not before: a `discard` that fails then leaves
  an orphan the second phase collects, where the other order would leave an active row whose
  session is gone and a PATCH that finds nothing to append to.
- Second, `IObjectAdmin::reap_abandoned` aborts every multipart upload in the bucket that began
  more than `ULW_UPLOAD_TTL_HOURS` (default 144, the gateway's `upload_ttl`) plus 24 hours ago.
  An active upload's session is never that old, so what it finds has no live row: the
  create-then-crash orphans, and sessions whose `discard` failed. The bound is why the sweep is
  safe to run against a bucket shared with live uploads; it is also why it is a second net, not
  the first. The count is `parts_orphaned_total`.
- Counters are printed to stdout in Prometheus text format at the end of a pass
  (`uploads_expired_total`, `parts_orphaned_total`): a process that lives for seconds has no
  endpoint to scrape, and each pass reports its own increment. Whatever collects the CronJob's
  output (a textfile collector, a Pushgateway) sums them.
- The bucket keeps a lifecycle rule aborting incomplete multipart uploads under `videos/` after
  7 days, as the backstop for a reaper that is not running. The RUNBOOK has the exact R2 rule.
  MinIO rejects a bucket lifecycle rule holding `AbortIncompleteMultipartUpload` (it answers
  `InvalidArgument`), and has a server-wide setting instead:
  `MINIO_API_STALE_UPLOADS_EXPIRY`, default 24 hours, which would abort uploads the gateway
  still allows. The sandbox sets it to 168 hours.

## Consequences

- An abandoned upload is failed and its session released within the CronJob's interval of its
  expiry (15 minutes), not 7 days.
- The sweep aborts whatever multipart upload it finds past the bound in the whole bucket. A
  second application writing multipart uploads into this bucket would lose them; the bucket is
  this system's alone.
- `ULW_UPLOAD_TTL_HOURS` duplicates the gateway's constant. A gateway that raised its TTL
  without the reaper being told would have its live sessions swept after the shorter bound. The
  gateway's TTL is not configurable today; when it becomes so, both read one variable.
- A reaper that is down for longer than the lifecycle rule's 7 days loses nothing but the
  bytes' early release; the rows are still failed on its return.
- Reopen if uploads gain a resume-after-expiry path, or if the pass outgrows a CronJob (a
  million abandoned sessions in one bucket listing).

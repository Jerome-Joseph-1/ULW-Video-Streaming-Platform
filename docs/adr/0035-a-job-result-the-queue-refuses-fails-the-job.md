# 0035. A job result the queue refuses fails the job

Status: Accepted
Date: 2026-09-29

## Context

`JobQueueError::Invalid` means the database refused the call's own values or statement: the
same call fails the same way every time (ADR-0006, `core/ports/job_queue.hpp`). The worker
treated every failed queue call as unrecorded: it left the job to its lease, which lapses, and
the reaper requeues it. For a refused `finish` that means two more full transcodes, each ending
in the same refusal, and then a video failed with "transcoding stopped responding", which is not
what happened.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Leave every failed call to the lease | Uniform | Rejected for `finish`: burns the retries on a certain failure and records the wrong reason |
| Retry the call | Covers a transient refusal | Rejected: `Invalid` is by definition not transient; `Unavailable` already covers what is |
| On a refused `finish`, fail the job now, not retryable | The job and its video fail once, with a reason that says the result could not be recorded | Accepted |
| Exit the worker | Makes a bug loud | Rejected: it would be restarted into the same refusal, and stop every other job meanwhile |

## Decision

- `finish` refused as `Invalid`: log an error and call `fail(reason = "the transcoded result
  could not be recorded", retryable = false)`. If that is written the outcome is `Failed`.
- `fail` refused, whatever the reason: nothing is left to try; outcome `Unrecorded`, and the
  lease and the reaper settle the job.
- A refused progress write is logged once and that value is not resent; a refused heartbeat or
  claim is logged and retried at the next beat or poll, as an outage is.
- `Unavailable` is logged as a warning, `Invalid` and `Corrupt` as errors.

## Consequences

- A bug that makes the database refuse a result costs one transcode, not three, and shows as an
  error line and a failed video with an honest reason.

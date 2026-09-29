# 0068. A job that fails for good fails its video in the same statement

Status: Accepted
Date: 2026-09-29

## Context

A video in `processing` has a job. If the job ends in `failed` and the video stays in
`processing`, the owner is told nothing, and playback answers 409 forever. ADR-0041 states the
rule for one case (a result the queue refuses) and ADR-0049 uses it for expired uploads ("as
the failed-job path does"); the general rule is written in SQL and in no decision.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| The worker fails the job, then the video, as two calls | Simple to read | Rejected: a worker that dies between them leaves a failed job and a video processing forever |
| A periodic sweep of videos whose job has failed | Catches every route | Rejected: delays the answer by the sweep interval and adds a second writer of `videos.state` |
| The statement that fails the job fails its video too | One statement, no window between them | Accepted |

## Decision

- Every transition of a job to its terminal `failed` state updates its video in the same
  statement, using a data-modifying CTE, to `state = 'failed'` with an `error_reason`, bumping
  `version` (`infra/postgres/src/job_queue.cpp`):
  - `kFail`, the worker's `fail`: the job becomes `failed` when the call is not retryable or its
    attempts are used up, and the video takes the worker's reason string. A retryable failure
    with attempts left requeues the job with a doubling delay and leaves the video in
    `processing`.
  - `kReap`, the lease reaper: a job whose lease lapsed with attempts exhausted becomes
    `failed`, and its video takes the fixed reason `transcoding stopped responding`. A lapsed
    lease with attempts left is requeued and touches no video.
- The video is updated only if it is still `processing` (`AND state = 'processing'`), so a video
  already `ready` or `failed` by another route is not overwritten.
- The `fail` is fenced like every worker write (`fence = $2`, ADR-0006): a worker that lost its
  lease fails nothing, in the job or the video.
- The upload reaper (ADR-0049) fails the video of an expired upload in the same way, with
  `upload expired`.

## Consequences

- A job and its video fail together or not at all, and every failed video carries a reason the
  owner can read.
- Code and brief agree. The brief's "in the same statement or transaction as the reaper update"
  is the single statement above. `tests/integration/postgres_job_queue_test.cpp` checks the
  video's state and reason after each route.
- The reason is a plain string, shown to the owner. The worker must not put anything into it
  that the owner should not see.

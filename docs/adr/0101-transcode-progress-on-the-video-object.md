# 0101. A processing video's transcode progress is a field of the video object, read from its job on every request

Status: Accepted
Date: 2026-10-09

## Context

Apps and the operator's backend show a video's status while it transcodes, and the only thing the
platform told them was `state: "processing"`, for anything from seconds to an hour. The worker
already knew more. It worked out a percent from ffmpeg's `out_time_us` against the probed
duration and wrote it to `jobs.progress_pct` every 10 s, when it had changed, fenced like every
other write the worker makes (ADR-0006). Nothing read that column.

The number had two flaws as a client-facing figure:

- **It reached 100 when ffmpeg exited.** Verification and the upload of every segment and
  playlist come after that, which for a long video takes minutes. A progress bar would sit at
  100% with the video still not playable.
- **A retry kept the last attempt's figure.** A claim did not reset the column, so a job
  requeued at 60% showed 60% until the new attempt's first write, then dropped.

What had to be settled:

- **How integrators receive it.** Clients already poll `GET /api/v1/videos/{id}` until `ready` or
  `failed` (videos-and-playback.md). Hosted video APIs work the same way: Cloudflare Stream's
  `status.pctComplete` and api.video's encoding status are read from the video, not pushed.
- **What it costs.** Progress is written every 10 s per running job, and read only when someone
  asks. The read must not add a round trip or a statement.
- **What it means** for each state, and for a job waiting to run again.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A `progress` field on the video object, read from the live job in the statement that reads the video | One field on the object clients already poll, on every route that answers a video (GET, the owner's and the service's PATCH, the service listing); no new write, connection or component; additive under versioning.md | Clients still poll | Accepted |
| Server-Sent Events from the gateway, fed by `NOTIFY` from the worker's progress write | No polling | Every watcher holds a gateway connection for the whole transcode; a `LISTEN` session per gateway; reconnects, catch-up and authorization of a long-lived stream; the gateway's per-connection memory budget (ADR-0027) is sized for short requests | Rejected for now; it can be added later and feed from the same column |
| Webhooks to the operator's backend | The backend learns without asking | An outbox table, a delivery loop with retries and signing, a URL to configure and protect from SSRF, for a value that changes every 10 s and is shown, not acted on | Rejected for progress; terminal events (ready, failed) are the case for webhooks, and are a decision of their own |
| A new column for the stage (downloading, encoding, publishing) | Finer labels | A migration and a second write per change, for labels a percent already orders | Rejected: two stages, read from the job's own state |

## Decision

- **The video object gains `progress`.** While the video is `processing` it is
  `{"stage": "queued" | "transcoding", "percent": 0..99}`; in every other state it is `null`.
  Everyone who may see the video sees it: it says nothing about whom the video is shared with.
  `GET /api/v1/videos/{id}`, `PATCH /api/v1/videos/{id}`, `PATCH /api/v1/service/videos/{id}` and
  each entry of `GET /api/v1/service/videos` carry it.
- **The stage is the job's state.** `queued`: no worker holds the job (the first attempt waiting
  to be claimed, or a retry in its backoff), and `percent` is 0, whatever an earlier attempt
  reached. `transcoding`: a worker holds the job, and `percent` is its last report, at most 99. A
  processing video with no live job, which the schema does not produce, reads `queued`.
- **100 is `ready`.** The worker maps the encode to 0-90 (`out_time_us` over the probed duration,
  capped at 90) and publishing to 90-99, by files uploaded over files to upload; the finish makes
  the video ready. The catalog caps what it reads at 99, so a value written by an older worker
  cannot say 100 before the video plays.
- **A claim resets `progress_pct` to 0**, in the claim's own `UPDATE`.
- **The read is the video's own statement.** `kFindVideoFor`, the two listing statements and
  `kSetVisibility` read the live job through `one_live_job` (`video_id, kind` where
  `state IN ('queued', 'running')`): a `LEFT JOIN LATERAL` in the selects, two scalar subqueries
  in the `RETURNING`. No statement or round trip is added. A video not in `processing` matches
  no job.
- **Progress is carried on `VideoRecord`** as an optional field nothing writes back, set by the
  reads that answer clients. Every path that writes a video object has it without a second
  type.

## Consequences

- Integrators show a progress bar from the object they already poll; the guide says how often to
  poll and what each field means (videos-and-playback.md#progress).
- The figure moves at most every 10 s (the worker's write interval) and the poll interval adds
  to that; a client wanting a smoother bar interpolates between reads.
- A retry shows `queued` at 0, then `transcoding` from 0 again: a bar can go back. A crash that
  ffmpeg is rerun for within the same attempt (ADR-0025's rerun) also starts the encode from 0.
- No new grant: the gateway's role already reads and writes `jobs` (ADR-0100's delete cancels
  queued jobs).
- Push (Server-Sent Events from the same column, or webhooks for ready and failed) stays open and
  would not change this field.

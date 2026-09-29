# 0054. A live recording is remuxed from its stored segments into one object, and queued once per stream

Status: Accepted
Date: 2026-09-29

## Context

M33: when a live stream ends, its recording becomes an ordinary video through the ordinary job
row, and the worker takes it from `processing` to `ready` without a line of it changing
(`git diff -- apps/worker/src` stays empty). The worker's input is one object: it asks the
store for the source's size, downloads it into its workspace (which needs three times that
free), probes it with ffprobe, and budgets the transcode's wall and CPU time from the probed
duration (ADR-0025). The recording therefore has to be one object whose timestamps rise from
start to end, or the budget is computed from the wrong duration and a long recording is killed
as over budget.

What exists when a stream ends (ADR-0047): the stored playlist with `EXT-X-ENDLIST`, the last
window only; every segment the packager published, `seg_<epoch>_<n>.m4s`, and each run's
`init_<epoch>.mp4`, all under `live/<stream>/` until the bucket's lifecycle rule expires them;
and the epoch claims. A stream the packager restarted has several runs, and each run's fMP4
timestamps start again at zero. The end can be seen more than once: the packager is killed
after the ENDLIST and started again, an operator sends SIGUSR1 to a stream that already ended,
or two packagers for one stream both finish. A stream is at most 12 hours at 20 Mbit/s
(ADR-0046), 108 GB.

## Options

**The recording source**

| Option | Why it was tempting | Verdict |
|---|---|---|
| A second output of the live ffmpeg (a TS file) | No re-read of the segments | Rejected: disk on the packager proportional to the stream (108 GB), and a crash loses it; a streamed upload during the stream would have to survive restarts, which a multipart upload id kept in memory does not |
| Concatenate the stored fMP4 (every init and segment) into the object | No ffmpeg, one pass | Rejected: tried with ffmpeg 6.1, a second run's `moov` is skipped as duplicated and its fragments are read on the first run's timeline, backwards; the probed duration is the first run's alone. Fine for one run, broken for a restarted stream |
| Server-side compose (UploadPartCopy) of the segments | Nothing through the packager | Rejected: every part but the last must be at least 5 MiB, and a 2 s segment is a few hundred KB |
| Give the worker a playlist of the segments | Nothing assembled | Rejected: the worker takes one object, ffmpeg runs without a network, and changing either touches `apps/worker/src` |
| `-output_ts_offset` per run, so the stored fMP4 is on one timeline | Fixes the concatenation at the source | Rejected: the offset is the stream's time up to the restart, which no stored state holds (the window's program date times are wall clock, and the window may not reach back to the start) |
| After the end, read the segments back in order through two stages of copying ffmpeg (each run's fMP4 to MPEG-TS, then all of it through one more TS-to-TS copy), and stream the output into the store as the object | ffmpeg treats a timestamp jump in a transport stream as a discontinuity and carries the timeline across it; the output rises throughout and probes to the full length. Nothing is proportional to the stream but time | Accepted |

**Which segment is the stream's at each place**

| Option | Why it was tempting | Verdict |
|---|---|---|
| Record every run's first sequence (in its claim) | Exact | Rejected: the claim is written before the run's second read of the playlist, so it is only a lower bound; and an old claim reads "claimed" |
| Walk from the ended window down, and take for each place the newest epoch that has a segment for it | Needs nothing new stored: epochs rise with the claims, each run continues where the stored playlist ended, so a newer epoch's segment is the published one and an older one's is what a superseded run cut before it saw the newer claim. One HEAD per segment | Accepted |

**Exactly once**

| Option | Why it was tempting | Verdict |
|---|---|---|
| The jobs table's `one_live_job` index | Exists | Rejected: it holds only while the job is queued or running; a second end after the video is ready would queue a second job |
| A deterministic video id from the stream id | No table | Rejected: breaks ADR-0023's UUIDv7, and says nothing about the job |
| A `live_recordings` row keyed by the stream id, inserted in the statement that inserts the video and the job, all hanging off it (`ON CONFLICT DO NOTHING RETURNING`) | Once per stream whatever the order of crashes and repeats; the answer names the stream's video | Accepted |

## Decision

- **Who records.** `live_packager`, after a run that ends the stream, and at start when the
  stored playlist has already ended (instead of refusing, which is what it does with recording
  off). A drain (SIGTERM) leaves the stream open and records nothing. It is configured by
  `ULW_DATABASE_URL` and `ULW_STREAM_OWNER`, both or neither; without them the packager is
  live-only, as in M31.
- **Owner and tenant.** The video belongs to `ULW_STREAM_OWNER`, the Askedin user id of the
  broadcaster, which whatever starts the packager for a stream (the component that owns stream
  lifecycles) passes in, as it passes the stream id. The platform is single-tenant: `owner_id`
  is the whole ownership model (ADR-0018), so the video is visible to the broadcaster only,
  like an upload, titled `Live stream <stream id>`.
- **The steps, each repeatable.** (1) If `live_recordings` has the stream, stop. (2) Read the
  stored playlist; not ended, or no segment: nothing to record. (3) If
  `live/<stream>/recording.ts` is not in the store, plan the runs and assemble it. (4) In one
  transaction, the `live_recordings` row, the video (`processing`), its `transcode` job with
  `source_key` the recording and `request_id` the stream id, and `NOTIFY job_available`. A
  packager killed anywhere in between is started again (its pod restarts on a non-zero exit)
  and resumes at the first step not done; two packagers racing both assemble, both
  commit whole objects of the same segments, and the row lets one insert.
- **Assembly.** For each run of the plan, a sandboxed `ffmpeg -f mp4 -i pipe:0 -map 0 -c copy
  -f mpegts pipe:1` is fed the run's init segment and its segments, downloaded one at a time
  into the packager's scratch; its stdout goes into the stdin of one sandboxed `ffmpeg -f mpegts
  -i pipe:0 -map 0:v:0 -map 0:a:0? -c copy -f mpegts pipe:1` for the whole recording, whose
  stdout is written to an object stream (`IObjectStreams`, a new port): a multipart upload of
  16 MiB parts held in memory on S3 and R2 (10,000 parts reach 168 GB), a temporary renamed
  over the key on the filesystem. The object appears only at the commit. Local disk: one
  segment; memory: one part. The children have no network, as ever (ADR-0025).
- **Holes.** A place no epoch has a segment for is counted in the log line and skipped; the
  recording goes on around it rather than never existing.
- **Budget.** The copies get the stream's maximum duration as their wall-clock budget, and a
  twentieth of it as CPU; copying runs at the store's speed, far faster than real time.

## Consequences

- The segments must outlive the stream by more than the recording takes and the job waits in
  the queue: the `live/` lifecycle rule is days, not hours. The recording lives under the same
  prefix and expires with it, after the worker has made its renditions.
- A bucket lifecycle rule aborting incomplete multipart uploads after a day collects the parts
  of a recording whose packager died mid-assembly (the next packager starts a new one).
- Recording costs one more read of the stream from the store after it ends, one HEAD per
  segment for the plan, and two copying ffmpeg processes, after the stream and off its path:
  the live window and its latency are untouched.
- A restarted stream's recording splices the runs: the gap while no packager ran is not in it.
- Nothing ends a stream that was drained and never started again (ADR-0047's stale-stream
  rule). Its recording follows whenever something starts a packager for it and sends SIGUSR1;
  the component that does that is still to be built.
- `apps/worker/src` is unchanged; the worker sees a job like an upload's, with a `.ts` source.

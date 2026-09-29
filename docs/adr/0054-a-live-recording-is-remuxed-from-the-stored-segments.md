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
and the epoch claims. A stream the packager restarted has several runs, each with its own init
segment, and each run's fMP4 timestamps start again at zero. A stream is at most 12 hours
(`ULW_LIVE_MAX_HOURS`) at up to 100 Mbit/s (`ULW_LIVE_MAX_KBPS`, default 20): 540 GB at the
ceiling, 108 GB at the default.

The end can be seen more than once: a packager killed after the ENDLIST and started again, an
operator's SIGUSR1 to an ended stream, two packagers finishing at once. It can also be seen
falsely: ADR-0047's fence is a check before each playlist write, not atomic with it, so a stale
packager A can pass its check, lose the stream to a newer B's claim, and still write an
ENDLIST that B overwrites at its next playlist write. A recording of that end would be a part
of the stream, and would take the stream's one video.

## Options

**The recording source**

| Option | Why it was tempting | Verdict |
|---|---|---|
| A second output of the live ffmpeg (a TS file) | No re-read of the segments | Rejected: disk on the packager proportional to the stream, and a crash loses it; a streamed upload during the stream would have to survive restarts, which a multipart upload id kept in memory does not |
| Concatenate the stored fMP4 (every init and segment) into the object | No ffmpeg, one pass | Rejected: tried with ffmpeg 6.1, a second run's `moov` is skipped as duplicated and its fragments are read on the first run's timeline, backwards; the probed duration is the first run's alone |
| Server-side compose (UploadPartCopy) of the segments | Nothing through the packager | Rejected: every part but the last must be at least 5 MiB, and a 2 s segment is a few hundred KB |
| Give the worker a playlist of the segments | Nothing assembled | Rejected: the worker takes one object, ffmpeg runs without a network, and changing either touches `apps/worker/src` |
| `-output_ts_offset` per run, so the stored fMP4 is on one timeline | Fixes the concatenation at the source | Rejected: the offset is the stream's time up to the restart, which no stored state holds |
| After the end, read the segments back in order through two stages of copying ffmpeg (each run's fMP4 to MPEG-TS, then all of it through one more TS-to-TS copy), and stream the output into the store as one object | ffmpeg treats a timestamp jump in a transport stream as a discontinuity and carries the timeline across it; the output rises throughout and probes to the full length. Nothing held grows with the stream | Accepted |

**Where the recording goes, and exactly once**

| Option | Why it was tempting | Verdict |
|---|---|---|
| A fixed key per stream (`live/<stream>/recording.ts`), reused if present | A crash after the upload skips the copy next time | Rejected: two recorders overwrite one key, possibly after a job has started from it; reuse assumes ffmpeg writes the same bytes twice, which nothing promises; and the `live/` lifecycle rule could expire a video's source |
| The jobs table's `one_live_job` index | Exists | Rejected: it holds only while the job is queued or running; a second end after the video is ready would queue a second job |
| A new video id per attempt, generated before the copy; the recording streamed to that video's source key (`videos/<id>/raw`, where an upload's source goes); a `live_recordings` row keyed by the stream id inserted in the statement that inserts the video and the job, all hanging off it (`ON CONFLICT DO NOTHING RETURNING`) | No two recorders share a key; the row is the only gate, and the answer names the stream's video; the source lives with the video, outside `live/` | Accepted |

**Which segment is the stream's at each place**

| Option | Why it was tempting | Verdict |
|---|---|---|
| Record every run's first sequence (in its claim) | Exact | Rejected: the claim is written before the run's second read of the playlist, so it is only a lower bound |
| Walk from the ended window down, and take for each place the newest epoch that has a segment for it | Needs nothing new stored: epochs rise with the claims, each run continues where the stored playlist ended, so a newer epoch's segment is the published one and an older one's is what a superseded run cut before it saw the newer claim. One HEAD per segment | Accepted |

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
- **The end must be the real one.** Before copying, and again after the copy and before the
  row: the playlist is read back and must still end, with the same last segment; and no claim
  `epoch_<N+1>` may exist, where N is the higher of the last segment's epoch and this process's
  own claim. This process's claim is the higher one when it published nothing itself: it
  claimed and then found the stream ended, or ended the stream (SIGUSR1, a publisher that never
  came or went silent) before a segment of its own, which leaves the previous run's window
  ended under its epoch. A claim above that belongs to a packager still publishing, whose own
  end records the stream; the run removes what it stored and exits 0.
- **The steps.** (1) If `live_recordings` has the stream, stop. (2) Read the playlist; not
  ended, or no segment: nothing to record. (3) The fence above. (4) Plan the runs; probe each
  run's init segment for its audio. (5) Generate a UUIDv7 video id, stream the recording to
  `videos/<id>/raw`, commit. (6) The fence again. (7) In one transaction, the row, the video
  (`processing`), its `transcode` job (source that key, `request_id` the stream id), and
  `NOTIFY job_available`. A run whose row does not go in (another recorder's did), or whose
  database is down, removes its object; the next run makes its own with a new id.
- **Failures.** One that may pass exits 1, and the pod's restart tries again from the first
  step: every error of the store (a credential, a bucket, a disk can be put right), the
  database, a sandbox that does not start, a copy killed by a signal (the OOM killer) or past
  its wall-clock or CPU budget, and a stop. One that cannot is written as the stream's row with
  no video and the reason, after the fence is checked again, and the packager exits 0, so
  nothing crash-loops and nothing records the stream afterwards: an init segment or a segment
  gone (the packager never deletes one, so it expired), ffmpeg or ffprobe exiting on its own
  with an error on the input, an unreadable playlist, and the recording past its bound, which
  the recorder counts itself rather than taking from the store. When one ffmpeg stage fails the
  other is stopped; the failure reported is the stage's that was not stopped. A place in the
  plan no epoch has a segment for is counted in the log and skipped.
- **A lost answer.** An insert whose answer is lost may have committed. Before removing its
  object after a failed insert, the run reads the stream's row again: a row naming its video
  means the insert went in, and the video is recorded; no row, or another video, and the
  object is removed; no answer at all, and the object is kept (at worst an unreferenced
  object, never a video without its source). A multipart completion retried after a lost
  answer finds the upload gone; the key is the stream's own, so an object of the stream's
  length under it is taken as the completion.
- **Assembly.** For each run of the plan, a sandboxed `ffmpeg -f mp4 -i pipe:0 -map 0:v:0
  -map 0:a:0? -c copy -f mpegts pipe:1` is fed the run's init segment and its segments,
  downloaded one at a time into the packager's scratch; its stdout goes into the stdin of one
  sandboxed `ffmpeg -f mpegts -i pipe:0 -map 0:v:0 -map 0:a:0? -c copy -f mpegts pipe:1` for the
  whole recording, whose stdout is written to an object stream (`IObjectStreams`, a new port).
  The children write into an empty directory of their own with a file-size limit of one byte;
  what the parent downloads is outside it.
- **Audio across runs.** The second stage takes its streams from what it sees first, so every
  run must carry the same ones. Each run's init segment is probed; when the stream has audio
  anywhere, a run without it is given silence of the first audio run's rate and layout
  (`anullsrc`, encoded to AAC alongside the copied video), and video and audio are mapped in
  that order in every run, so the TS PIDs match.
- **Splices.** At a run boundary ffmpeg offsets every stream by the jump it sees in the first
  packet after it. A run's audio and video rarely end together, so each splice can shift audio
  against video by up to one audio frame plus one video frame (about 55 ms at 48 kHz AAC and
  30 fps); the gap while no packager ran is not in the recording.
- **Size and memory.** The recording is bounded by the stream's cap for its maximum duration
  plus an eighth for the TS packets and a stand-in's audio (607.5 GB at 100 Mbit/s for 12 h).
  S3 and R2 take it as a multipart upload whose part is that bound over 9,000 parts, rounded up
  to a whole MiB, at least 16 MiB: 16 MiB at the default 20 Mbit/s, 65 MiB at the ceiling. One
  part is held in memory at a time. A stream written past its bound fails (Permanent). The
  filesystem backend writes a temporary beside the key and renames it.
- **Budget.** Each copy gets the stream's maximum duration as its wall-clock budget, and CPU
  time from the recording's bound: measured with ffmpeg 6.1 on 121 MB of 720p at 8 Mbit/s,
  the fMP4-to-TS copy took 0.47 CPU-seconds and the TS-to-TS copy 0.60, pipe I/O included, so
  at most 5 CPU-seconds per GB; four times that and a minute, 3.4 hours at the 607.5 GB
  ceiling. (A twentieth of the wall clock, 36 minutes, would not have covered the ceiling.)

## Consequences

- A run killed after its commit and before its row leaves `videos/<id>/raw` with no video row.
  Nothing reads it; nothing yet deletes it either (the upload reaper works from upload rows).
  It costs the bytes of one recording per such crash.
- An incomplete multipart upload of a recorder that died mid-copy is collected by the upload
  reaper's sweep and the bucket's rule for `videos/` (ADR-0049, both at 7 days); a recording's
  own upload never lives that long.
- The segments must outlive the stream by more than the copy takes: the `live/` expiry is days,
  not hours. The recording itself is under `videos/`, so that rule never touches a video's
  source.
- A claim above the ended playlist whose holder died between its claim and its read of the
  playlist blocks the recording for good: every later run sees a newer claim that is not its
  own, and no packager publishes to an ended stream. The packager logs `superseded: epoch N
  is claimed` and exits 0; removing that claim object lets the next start record the stream.
- The worker downloads the whole recording into scratch that needs three times its size: 10 GiB
  of recording on the production worker's 30 GiB (about 70 minutes at 20 Mbit/s, 4.8 hours at
  5 Mbit/s). A larger one fails its job after its attempts, the video `failed` with `no scratch
  space for the source`, as an upload that size would.
- Recording costs one more read of the stream from the store after it ends, one HEAD per
  segment for the plan, and two copying ffmpeg processes, after the stream and off its path:
  the live window and its latency are untouched.
- Nothing ends a stream that was drained and never started again (ADR-0047's stale-stream
  rule). Its recording follows whenever something starts a packager for it and sends SIGUSR1;
  the component that does that is still to be built.

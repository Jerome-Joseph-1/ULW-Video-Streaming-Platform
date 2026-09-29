# 0038. The live playlist is a window the packager owns, and it continues across a restart

Status: Accepted
Date: 2026-09-29

## Context

Viewers fetch a live media playlist about once per target duration (ADR-0014) and need three
properties of it: `EXT-X-MEDIA-SEQUENCE` never decreases, everything it lists is already in the
store, and it ends with `EXT-X-ENDLIST` when the stream does. The packager can crash, or be
stopped and started again, in the middle of a stream; a sequence that restarted at 0 would make
every player treat the stream as new.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Upload ffmpeg's own playlist | Nothing to write | Rejected: its program date times start from a whole second, its numbering restarts with the process, and it writes ENDLIST on its own schedule; a restart would go back to sequence 0 |
| Keep the state in a local file | Fast, no store read | Rejected: a rescheduled pod has a new disk; the store already holds the truth, and what viewers can see is what counts |
| Poll a directory for new `.m4s` files | No playlist parsing | Rejected: a file that exists may still be growing; only ffmpeg knows it is closed |
| Take completeness from ffmpeg's playlist (a segment is listed only once closed, and is written under `.tmp` until then), keep our own window, and on start read the stored playlist to continue it | The store is the only state; a restart cannot go backwards | Accepted |

## Decision

- A segment is complete when ffmpeg's playlist lists it and its file exists and is not empty.
  Segments are handed out in order; one whose file is missing holds back the ones after it.
  Segments are named by their sequence number (`seg_<n>.m4s`, `-start_number`), and the scan
  fails when a listed name disagrees with `media_sequence + index`, or when ffmpeg's list has
  moved past a segment not yet uploaded (its list holds twice the window, and the packager gives
  up on uploads after one window).
- The published playlist is the last N segments (default 10, 3 to 64), with `EXT-X-VERSION:7`,
  the target duration from configuration, and `EXT-X-PROGRAM-DATE-TIME` on every segment: the
  wall clock at the publisher's connection for the first, then each segment's start plus the
  duration before it. Segments that slide out raise `EXT-X-MEDIA-SEQUENCE` by one each.
- Publishing a segment is: upload the init segment (once per run), upload the segment, then
  upload the playlist that lists it. A failed segment upload changes nothing visible and is
  retried at the next look; a failed playlist upload is retried without uploading segments
  again. Playlist writes are one per segment, inside R2's one write per second per key for any
  target duration over one second (the configuration's lower bound is 2 s).
- On start, `live/<stream>/index.m3u8` is read from the store. Absent: a new stream, sequence 0.
  Unreadable or not ours: the packager refuses to start rather than risk restarting at 0.
  Ended: refused, an ended stream is not continued. Otherwise the window is resumed: numbering
  continues at `media_sequence + count`, the run writes `init_<epoch+1>.mp4`, and its first
  segment carries `EXT-X-DISCONTINUITY` and a new `EXT-X-MAP`, with
  `EXT-X-DISCONTINUITY-SEQUENCE` counting the ones that have left the window. The target
  duration stays that of the stored playlist. A segment uploaded just before a crash and never
  listed is overwritten by the new run's segment of the same number.
- The stream ends, with `EXT-X-ENDLIST` and the last segment ffmpeg finalises, when the publisher
  disconnects, on SIGTERM or SIGINT, or at the maximum duration (12 h, the longest video the
  platform takes). SIGKILL ends nothing, which is what lets the next process continue.
  A connection that sends no complete segment for five target durations is given up on, and a
  stream that breaks (ffmpeg fails, uploads fail for a window) is ended the same way and exits
  non-zero.
- Objects under `live/<stream>/` are objects in the store with no deletion by the packager
  (`IObjectTransfer` has none): a bucket lifecycle rule on the `live/` prefix expires them. A
  recording for M33 is a separate output of the stream, not these segments.

## Consequences

- The store is read once per start and never during a run.
- One packager per stream. Nothing fences a second writer to the same prefix; the component that
  starts packagers must (a Deployment of one replica, or a lease, when the egress is wired).
- A viewer partway through a discontinuity sees the timeline break where the crash happened; a
  player that mishandles discontinuities would stall there. hls.js plays through it.
- Logs are one line per event plus one per thirty segments; the ffmpeg stderr tail is bounded
  at 4 KiB by the sandbox runner. No line carries the credentials.
- The gateway's `LiveManifestCache` (M31, after the ops lane) reads `index.m3u8` and rewrites it
  as it does VOD playlists (ADR-0024); this ADR fixes its shape, not that code.

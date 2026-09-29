# 0047. The live playlist is a window the packager owns, and it continues across a restart

Status: Accepted
Date: 2026-09-29

## Context

Viewers fetch a live media playlist about once per target duration (ADR-0014) and need three
properties of it: `EXT-X-MEDIA-SEQUENCE` never decreases, everything it lists is already in the
store, and it ends with `EXT-X-ENDLIST` when the stream does. It must also keep the rules of
RFC 8216: every segment's duration, rounded to the nearest second, is at most
`EXT-X-TARGETDURATION` (4.3.3.1), and the target duration does not change during a stream
(6.2.1). The packager can crash, be drained, or be started twice for one stream; a sequence
that restarted at 0 would make every player treat the stream as new, and two writers to one
prefix would overwrite each other's listed objects.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Upload ffmpeg's own playlist | Nothing to write | Rejected: its program date times start from a whole second, its numbering restarts with the process, and it writes ENDLIST on its own schedule |
| Keep the state in a local file | Fast, no store read | Rejected: a rescheduled pod has a new disk; the store already holds the truth |
| Poll a directory for new `.m4s` files | No playlist parsing | Rejected: a file that exists may still be growing; only ffmpeg knows it is closed |
| Publish a segment that runs past the target duration | The stream keeps going | Rejected: the playlist would break a MUST, and TARGETDURATION cannot be raised later |
| A compare-and-swap on the playlist (If-Match) to fence writers | Stronger than a claim | Deferred: it needs an opaque version token in the transfer port; the create-only claim below needs only one added call |
| Take completeness from ffmpeg's playlist (a segment is listed only once closed, and is written under `.tmp` until then), keep our own window, claim an epoch per run with a create-only put, and read the stored playlist to continue it | The store is the only state; a restart cannot go backwards; a stale writer cannot clobber a newer one | Accepted |

## Decision

- **Complete segments.** A segment is complete when ffmpeg's playlist lists it and its file
  exists and is not empty. Segments are handed out in order; one whose file is missing holds
  back the ones after it. Segments are named `seg_<epoch>_<n>.m4s` (`-start_number`), the scan
  fails when a listed name disagrees with the epoch or with `media_sequence + index`, and when
  ffmpeg's list has moved past a segment not yet uploaded (its list holds twice the window).
- **The playlist.** The last N segments (default 10, 3 to 64) with `EXT-X-VERSION:7`, the
  configured target duration, and `EXT-X-PROGRAM-DATE-TIME` on every segment: the wall clock
  at the publisher's **first media** (the first payload read from the SRT session, not the
  connection, which may precede the media) for the first, then each segment's start plus the
  duration before it. Segments that slide out raise `EXT-X-MEDIA-SEQUENCE` by one each.
- **The target duration is the segment length T**, and it is stored in the playlist. The
  contract allows a segment up to `T + 0.5 s` (rounding, RFC 8216 4.3.3.1): `kSegmentDriftAllowance`
  is that half second, the drift a source's keyframe interval may have. A segment past it is not
  published; the stream is ended, with `EXT-X-ENDLIST`, reporting "keyframe interval exceeds the
  contract". A restart with another segment length than the stored playlist's is refused.
- **Publishing.** Upload the init segment (once per run), the segment, then the playlist that
  lists it. A failed segment upload changes nothing visible and is retried at the next look; a
  failed playlist upload is retried without uploading segments again. Playlist writes are one
  per segment, inside R2's one write per second per key for any target duration over one
  second (the configuration's lower bound is 2 s).
- **Failure clocks are separate.** A stream is given up on when ffmpeg finishes no segment for
  five segment lengths (measured from ffmpeg's playlist growing, whatever the store does), or
  when the store refuses uploads for one window's worth of segments (`listed / 2 * T`, before
  ffmpeg's list of twice the window can drop a segment under the uploader). A store outage
  shorter than that is survived, and the segments cut during it go up after it.
- **Epochs fence writers.** A run claims epoch E with a create-only put of
  `live/<stream>/epoch_<E>` (the object transfer port's `upload_new`), the smallest free above
  the last epoch in the stored playlist; claims left by runs that never published are stepped
  over. The playlist is read again after the claim, and the claim goes higher if a newer epoch
  published meanwhile, so a start never continues from a stale copy. The newest claim is the
  writer: before each playlist write a run checks that `epoch_<E+1>` does not exist, and stops
  with "superseded" (no ENDLIST, exit 1) if it does. Segments and init segments carry the epoch
  in their names, so a stale writer never overwrites an object a newer playlist lists. The check
  is not atomic with the write: a stale writer can land one more playlist, which the newer
  run overwrites at its first; the If-Match option above would close that.
- **Restart.** On start `live/<stream>/index.m3u8` is read. Absent: a new stream, sequence 0.
  Unreadable or not ours: refused, rather than risk restarting at 0. Ended: refused. Otherwise
  the window is resumed: numbering continues at `media_sequence + count`, the run writes
  `init_<E>.mp4`, and its first segment carries `EXT-X-DISCONTINUITY` and a new `EXT-X-MAP`,
  with `EXT-X-DISCONTINUITY-SEQUENCE` counting the ones that have left the window.
- **Ending versus draining.** The publisher disconnecting, SIGUSR1 (the explicit end), the
  maximum duration (12 h) and a broken stream (ffmpeg fails, uploads fail for a window, a
  keyframe interval past the contract, segments lost or playlists inconsistent) end the stream:
  what has been published is followed by `EXT-X-ENDLIST`, whatever went wrong, unless the run was
  superseded. **SIGTERM and SIGINT drain**: the last segments ffmpeg finished are uploaded and
  the process exits 0 without ENDLIST, leaving the stream to be continued by the next start of
  that stream id, as after a SIGKILL. A node drain therefore does not end a stream that
  is still being published to its replacement. A stream abandoned by a drain and never started
  again keeps its last playlist without ENDLIST; ending it is a stale-stream rule for the
  component that owns stream lifecycles (M33: no playlist change for a multiple of the target
  duration, then a packager started for that stream id and sent SIGUSR1, which ends the resumed
  window without a publisher).
- **Objects.** Objects under `live/<stream>/` are never deleted by the packager
  (`IObjectTransfer` has none): a bucket lifecycle rule on the `live/` prefix expires them. A
  recording for M33 is a separate output of the stream, not these segments.

## Consequences

- The store is read at start and never during a run; a run costs one extra HEAD per playlist
  write for the fence.
- A viewer partway through a discontinuity sees the timeline break where the crash happened; a
  player that mishandles discontinuities would stall there. hls.js plays through it.
- Logs are one line per event plus one per thirty segments; the ffmpeg stderr tail is bounded
  at 4 KiB by the sandbox runner. No line carries a credential or the passphrase.
- The gateway's `LiveManifestCache` (M31, after the ops lane) reads `index.m3u8` and rewrites it
  as it does VOD playlists (ADR-0024); this ADR fixes its shape, not that code.

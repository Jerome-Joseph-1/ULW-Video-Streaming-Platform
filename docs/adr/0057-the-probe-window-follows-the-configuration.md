# 0057. The packager's probe window follows its configuration

Status: Accepted
Date: 2026-09-30

## Context

ADR-0046 has the packager's ffmpeg read the publisher's MPEG-TS from a pipe and copy it into
fMP4 HLS, "probing for 1 s rather than the default 5 s". ffmpeg writes nothing, not even the
init segment, until it knows every mapped stream's parameters, and for H.264 the picture size
comes with a keyframe. The publisher sends a keyframe every segment length T (2 to 10 s,
ADR-0046), but it joins wherever its encoder is: the first keyframe can reach the packager up
to T after the first audio.

A WHIP publish in CI (`tests/call/ingest.spec.mjs`, "a WHIP publish becomes a live playlist
that ends with the session") produced no segment in 60 s. Its first video keyframe came after
the first second of audio, so ffmpeg printed "Could not find codec parameters for stream 0
(Video: h264 ...): unspecified size", then "[mp4] dimensions not set", could not write the
header and exited 234. The run ended, the SRT session dropped ("Socket is broken or closed"),
and egress failed with 503. ffmpeg's last line, the one the packager logged, was only "Error
opening output files: Invalid argument".

Measured with ffmpeg 6.1.1, piping an MPEG-TS whose audio starts at 0 (640x360, a keyframe
every 2 s) into the packager's command, the window is the bound: with 1 s, video starting
0.8 s in was packaged and 1.0 s in was not; with 3 s, 2.8 s in was and 3.0 s was not.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Keep 1 s | The least delay before the first segment | Rejected: it fails every publisher whose first keyframe is more than a second behind its audio, which with 2 s keyframes is about half of those that join at a random point in the interval |
| ffmpeg's default, 5 s and 5 MB | No number of ours | Rejected: short of a 10 s keyframe interval, and 5 MB is under 3 s at the 20 Mbit/s default |
| A keyframe interval and a second, from the configuration | Holds a keyframe whatever T is, and costs latency only where T asks for it | Accepted |
| Restart ffmpeg on the same SRT session when it exits for want of codec parameters | Keeps the publisher connected | Rejected: once the window holds a keyframe interval, a publisher that fails it breaks the keyframe contract, and a second ffmpeg probes the same stream the same way; it would also need a second pipe handed to the relay, and a new epoch |

## Decision

- `-analyzeduration` is T + 1 s: the keyframe interval, and a second for the relay's start and a
  keyframe late on the interval.
- `-probesize` is that window at `ULW_LIVE_MAX_KBPS`, twice (the headroom of ADR-0046's file
  size limit), and never under the 1 MB it was.
- Both are capped: T at 10 s and the bitrate at 100 Mbit/s, the most the configuration accepts
  (`infra::ffmpeg::kLiveMaxSegmentSeconds` and `kLiveMaxKbps`, which the configuration's own
  limits are). The most ffmpeg is told is 11 s and 275 MB. At the defaults, 3 s and 15 MB.
  `infra::ffmpeg::live_probe` computes them, and the remuxer passes the same values to the
  command line and to its result.
- When ffmpeg fails and its stderr has the line "Could not find codec parameters for stream ...
  (Video: ...", the packager logs "ffmpeg found no video codec parameters in the first <ms> ms
  (<bytes> bytes) of the stream, which must hold a keyframe: the publisher is to send one every
  segment length", with the values ffmpeg was given. The line is not logged for a run that went
  on, nor for another stream: ffmpeg reports an unmapped data stream such as SCTE-35 the same
  way and carries on.
- ffmpeg is not restarted for it; the stream ends as any failed run does (ADR-0047).

## Consequences

- The first segment comes later. On a real-time feed with 2 s keyframes it was listed 2.0 s
  after the first byte with 1 s of probe and 2.8 s with 3 s. Segments after the first are not
  delayed: ffmpeg reads the backlog faster than real time.
- ffmpeg holds what it probed in memory until it writes the header: the window at the
  publisher's actual bitrate, 7.5 MB for 3 s at the 20 Mbit/s default, and about 137 MB in
  the worst case (11 s at 100 Mbit/s), within the child's 1 GiB address space.
- `LivePackagerTest.AStreamWhoseFirstKeyframeComesLateInTheSegmentIsStillPackaged` sends video
  1.8 s behind its audio over SRT and expects the stream packaged; it fails with the 1 s window.
- ADR-0046's "probing for 1 s" is replaced by this window; the rest of 0046 stands.

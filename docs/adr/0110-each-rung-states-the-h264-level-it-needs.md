# 0110. Each rung states the H.264 level its size and the source's frame rate need

Status: Accepted
Date: 2026-10-14

## Context

The transcode passed `-level 4.0` for every rung and keeps the source's frame rate. Level 4.0
allows 245,760 macroblocks a second; a 1080p frame is 8,160, so 4.0 holds 1080p up to 30 fps. A
1080p60 upload came out at 489,600 a second, labelled 4.0 (`avc1.4d4028` in the master
playlist). x264 warns (`MB rate (489600) > level limit (245760)`) and encodes anyway. A decoder
sizes itself from the level it is told, and a hardware decoder (TVs, set-top boxes, older phones)
may refuse or drop frames on a stream that exceeds it. An ultra-wide 1080p (2560x1080, 10,880
macroblocks a frame) is past 4.x's frame size at any rate.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Cap the output at 30 fps | One level for everything | Halves the motion of every 50/60 fps upload (sport, games, screen captures), and the 4 s GOP arithmetic must follow the new rate | Rejected |
| Raise the fixed level to 4.2 | One token | Still wrong past 1080p60 or for wide frames, and labels every 30 fps stream higher than it is | Rejected |
| Each rung states the lowest level from 4.0 up that holds its size, the source's rate and its VBV | Correct for any source; unchanged for everything 4.0 holds | A table of Table A-1's limits to keep | Accepted |

## Decision

- `h264_level` picks the lowest Main-profile level from 4.0 up whose limits (ITU-T H.264
  Table A-1: macroblocks a frame and a second, the bound on each side of a frame, peak bitrate,
  buffer) hold the rung. The width is the one `scale=-2:height` gives the displayed source.
- 4.0 is the floor: every rung was labelled 4.0 before, and lowering a level that is already
  honest would change the `CODECS` string of every 30 fps ladder for no decoder's benefit.
- Past 6.2 (over about 2,000 fps at 1080p, which is a broken `r_frame_rate` rather than a video)
  the rung is labelled 6.2 and x264 warns, as it did before for any overrun.
- The level is passed per output stream (`-level:v:N`); ffmpeg's HLS muxer writes each variant's
  `CODECS` from its own SPS, so the master playlist follows (`avc1.4d402a` for a 1080p60 rung).

## Consequences

- 1080p50 and 1080p60 rungs are labelled 4.2; 1080p120 is 5.1; 720p and 360p rungs at up to
  60 fps stay 4.0. Every source 4.0 already held produces the same command as before.
- Videos transcoded before this keep their 4.0 label until they are transcoded again.

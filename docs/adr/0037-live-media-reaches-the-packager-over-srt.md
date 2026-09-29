# 0037. Live media reaches the packager over SRT, which it terminates itself

Status: Accepted
Date: 2026-09-29

## Context

The live packager (`apps/live-packager`, brief M31) turns a room's media into fMP4 HLS. LiveKit
egress (ADR-0020) can send a room composite to a stream URL over RTMP or SRT; it does not emit
bare RTP. Whatever the transport, ffmpeg does the remuxing, and ffmpeg parses hostile-shaped
input, so it runs in the sandbox of ADR-0025: an empty network namespace, where it can neither
listen for an RTMP publisher nor connect to an SRT peer.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| `ffmpeg -listen 1 -i rtmp://...` receiving LiveKit's RTMP directly | The transport egress speaks; no relay | Rejected: ffmpeg would need a network, and the sandbox's empty network namespace is the property that keeps a decoder exploit off the cluster network (ADR-0025). Dropping it for one child to parse the protocol as well as the media reverses that |
| SRT or RTP from the SFU into ffmpeg over UDP | Low latency; RTP is what the SFU forwards | Rejected: same network need, RTP needs an SDP file and has no loss recovery, and egress does not emit RTP |
| LiveKit egress writing HLS to the bucket itself | No packager at all | Rejected: the playlist window, its sequence across a restart, `EXT-X-PROGRAM-DATE-TIME` for latency measurement and the write rate against R2's one write per second per key (ADR-0014) stay out of our hands |
| Re-encode in the packager (decode, x264, AAC) | Any input codec, keyframes placed where we want them | Rejected: 35 times the CPU of a copy (3.6 against 0.1 CPU-seconds for the same 30 s at 640x360, libx264 veryfast, and the gap grows with the picture) for a stream that is already H.264 and AAC; the one thing a copy needs from the source, a keyframe every segment, is an encoder setting on the source |
| MPEG-TS on TCP to a listener in the packager process, which hands the accepted socket to the sandboxed ffmpeg as its stdin, then `-c copy` into fMP4 HLS | The child keeps its empty network namespace: it reads a descriptor, not a network. The packager's part of the network stays a few lines of `accept` | Accepted |

## Decision

- `live_packager` binds one TCP listener (loopback unless `ULW_LIVE_INGEST_HOST` says
  otherwise), accepts the first connection, and closes the listener: one publisher, one stream.
  The connection becomes the ffmpeg child's stdin (`run_sandboxed` takes an optional input
  descriptor; the sandbox helper still closes every descriptor above 2). The packager never reads
  the bytes.
- ffmpeg runs `-f mpegts -i pipe:0 -map 0:v:0 -map 0:a:0? -c copy -bsf:a aac_adtstoasc -f hls
  -hls_segment_type fmp4 -hls_time <T>`, probing for 1 s rather than the default 5 s. The
  segment length T is fixed by configuration (2 to 10 s, default 2 s).
- A remux cuts at keyframes, so the source's keyframe interval must equal T. A segment that
  rounds to more than T is counted and logged at the end; TARGETDURATION does not change during
  a stream. The test publisher (`apps/live-packager/testsource/ulw-live-testsource`) encodes a
  keyframe every T seconds; the egress request sets `keyframe_interval` alike.
- Bridging egress's RTMP or SRT output to this listener is the wiring step after the SFU (M25):
  an ffmpeg copy `-c copy -f mpegts tcp://packager` in the egress pod, outside this process and
  its sandbox, since it sits inside the trust boundary that already holds the room's media.

## Consequences

- Measured on the test publisher (640x360, 1.5 Mbit/s video and 64 kbit/s audio): the remux
  costs 0.1 CPU-seconds per 30 s of stream (0.3% of a core) and 58 MB resident. A packager for
  one stream fits a small pod.
- Glass-to-glass latency, measured in hls.js in Chrome for Testing 141 by reading the frame's
  own creation time back out of the picture and, independently, from `EXT-X-PROGRAM-DATE-TIME`
  (`tests/e2e/live-run.sh`): a viewer joining a stream that is already running sits about 7.1 s
  behind (both methods, p95 7.3 s), which is what the viewer's three-target-duration start
  (RFC 8216, section 6.3.3) plus the segment being cut (up to T) and uploaded predicts; a viewer
  who joins with the first segment, before the window is full, starts nearer the beginning and
  sees 3.3 s. ADR-0014 accepts this class of latency.
- The packager needs the same host features as the worker (user namespaces, ADR-0032), and runs
  in the same kind of pod.
- Nothing authenticates the publisher. On loopback that is the whole trust model; a packager
  listening on a pod address is protected by a NetworkPolicy that admits the egress pod only,
  which the deployment manifests must carry.
- A publisher that disconnects ends the stream (ADR-0038). A network blip between egress and
  packager therefore ends it too, until the egress side reconnects into a new stream.
- Reopen if ffmpeg must terminate the transport itself (SRT with retransmission across a
  lossy link): that needs a network for the child, or a receiver we write.

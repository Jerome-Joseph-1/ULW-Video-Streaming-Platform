# 0046. Live media reaches the packager over SRT, which it terminates itself

Status: Accepted
Date: 2026-09-29

## Context

The live packager (`apps/live-packager`, brief M31) turns a room's media into fMP4 HLS. The
brief allows "LiveKit egress to RTP or RTMP". Read from LiveKit egress v1.14.1 and its protocol
v1.52.1, egress streams to a URL over **RTMP (FLV) or SRT (MPEG-TS, H.264 and AAC) only**: the
accepted schemes are rtmp, rtmps, mux, twitch, srt, ws and wss, `StreamProtocol` has four
values (default, RTMP, SRT, WebSocket), and WebSocket output is raw PCM audio. There is no RTP
egress, and no tcp:// or udp:// one; the RTP of the brief's option does not exist. RoomComposite,
Participant and TrackComposite requests can stream; a Track request cannot. `key_frame_interval`
defaults to 4 s for streaming.

Whatever arrives, ffmpeg does the remuxing, and ffmpeg parses input that came from a network
peer, so it runs in the sandbox of ADR-0025: an empty network namespace, where it can neither
listen for an RTMP publisher nor connect to an SRT peer.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| ffmpeg terminates RTMP (`-listen 1`) or SRT itself | Nothing of ours between egress and ffmpeg | Rejected: it needs a network, and the empty network namespace is what keeps a decoder exploit off the cluster network. It would be a second exception to ADR-0025 for the process that parses the hostile bytes |
| A relay in the egress pod, `ffmpeg -c copy -f mpegts tcp://packager` | Keeps the packager's socket a bare TCP accept | Rejected: egress is a pooled deployment that takes requests at random, so a sidecar there cannot know which packager a stream belongs to; and the relay is an unsandboxed ffmpeg running the TS demuxer on the network |
| Terminate RTMP in the packager | The protocol egress speaks by default | Rejected: a hand-written RTMP handshake, AMF and chunk parser in an unsandboxed process, to end up with the FLV that ffmpeg would have read |
| `srt-live-transmit srt://:P?mode=listener file://con` spawned by the packager | No library in our process | Rejected: it needs a sandbox mode with a network, for one more process, and a pinned build of libsrt's apps besides |
| libsrt in the packager: an SRT listener, the payload written to a pipe that is ffmpeg's stdin | SRT is the only TS-bearing egress output. Egress connects straight to a per-stream listener, no relay. The packager parses SRT and never the media; ffmpeg keeps its empty network namespace, so ADR-0025 needs no exception | Accepted |
| LiveKit egress writing HLS to the bucket itself | No packager | Rejected: the playlist window, its sequence across a restart, `EXT-X-PROGRAM-DATE-TIME` for latency measurement and the write rate against R2's one write per second per key (ADR-0014) stay out of our hands |
| Re-encode in the packager | Any input codec, keyframes where we want them | Rejected: 35 times the CPU of a copy (3.6 against 0.1 CPU-seconds for the same 30 s at 640x360, libx264 veryfast, and the gap grows with the picture) for a stream that is already H.264 and AAC; the one thing a copy needs, a keyframe every segment, is an encoder setting on the source |

## Decision

- `live_packager` runs an SRT **listener** (libsrt 1.5.4, `third_party/srt-1.5.4.tar.gz`, pinned
  by SHA-256 `d0a8b600fe1b4eaaf6277530e3cfc8f15b8ce4035f16af4a5eb5d4b123640cdd` like the other
  vendored sources, built static against OpenSSL) on a UDP port. It is the only network-facing
  code of the stream. The egress request is: caller mode, `srt://<packager>:<port>` with a
  `passphrase` and a `streamid` in the URL, `key_frame_interval` equal to the segment length.
- **Authentication is at the SRT hop.** The listener requires encryption with the passphrase
  (`ULW_LIVE_SRT_PASSPHRASE`, 10 to 79 characters, a secret that is never logged) and a stream id
  equal to the stream's own. Callers without them, or with another stream id, are refused during
  the handshake and the listener goes on waiting; a port scan or a probe does not become the
  publisher. The listening socket closes with the first caller accepted: one publisher, one
  stream.
- Each payload read from the SRT session is written to a pipe, and the pipe is ffmpeg's stdin
  (`run_sandboxed` takes an optional input descriptor; the helper still closes every descriptor
  above 2). The pipe is one-way, so a compromised ffmpeg cannot write back to the publisher. The
  packager never reads the bytes. When the caller is gone the pipe is closed, which is ffmpeg's
  end of input.
- ffmpeg runs `-f mpegts -i pipe:0 -map 0:v:0 -map 0:a:0? -c copy -bsf:a aac_adtstoasc -f hls
  -hls_segment_type fmp4 -hls_time <T>`. The segment length T is fixed by configuration (2 to
  10 s, default 2 s).
- **The probe window follows the configuration.** ffmpeg writes nothing until it knows every
  stream's parameters, and for H.264 that takes a keyframe. The publisher joins mid-interval, so
  its first keyframe can come up to T after its first audio. `-analyzeduration` is therefore
  T + 1 s (the keyframe interval plus a second for the relay's start and a late keyframe), and
  `-probesize` is that window at `ULW_LIVE_MAX_KBPS`, twice, never under 1 MB
  (`infra::ffmpeg::live_probe`). Both are clamped to the configuration's bounds, 11 s and 11 s
  at 100 Mbit/s twice (275 MB), whatever they are given. At the defaults: 3 s and 15 MB.
- A copy cuts at keyframes, so the source's keyframe interval must equal T. A segment that
  breaks the target duration ends the stream (ADR-0047).
- The sandbox limits the size of a file ffmpeg writes (`RLIMIT_FSIZE`, ADR-0025 gains a flag,
  off unless asked for): a segment at the configured maximum bitrate (`ULW_LIVE_MAX_KBPS`,
  default 20 Mbit/s, four times the worker's 1080p rung) at the longest the contract allows
  (T + 0.5 s), twice. A publisher that never sends a keyframe cannot fill the disk.

## Consequences

- The packager holds libsrt and its parsing of UDP from the network in an unsandboxed process.
  That is the exposure ADR-0025 keeps out of ffmpeg; here it is libsrt's handshake and packet
  code, behind a passphrase, on a port that a NetworkPolicy should admit egress to only. libsrt
  is pinned, and the version is in the third-party table.
- Measured on the test publisher (640x360, 1.5 Mbit/s video and 64 kbit/s audio): the remux
  costs 0.1 CPU-seconds per 30 s of stream (0.3% of a core) and 58 MB resident. A packager for
  one stream fits a small pod.
- Glass-to-glass latency, measured in hls.js in Chrome for Testing 141 by reading the frame's
  own creation time back out of the picture and, independently, from `EXT-X-PROGRAM-DATE-TIME`
  (`tests/e2e/live-run.sh`), over SRT from ffmpeg as the caller: a viewer joining a stream that
  is already running sits about 7 s behind (the three-target-duration start of RFC 8216, section
  6.3.3, plus the segment being cut and uploaded); a viewer who joins with the first segment,
  before the window is full, starts nearer the beginning and sees about 3.3 s. ADR-0014 accepts
  this class of latency.
- The probe window was a fixed 1 s until a WHIP publish in CI produced no segment: its first
  video keyframe came after the first second of audio, ffmpeg found no picture size ("Could not
  find codec parameters ... unspecified size", then "[mp4] dimensions not set"), could not write
  the init segment and exited 234, which ended the SRT session and the egress with it. Measured
  with ffmpeg 6.1.1 by piping an MPEG-TS whose audio starts at 0 (640x360, a keyframe every
  2 s) into the packager's command: the window is the bound. With 1 s, video starting 0.8 s in
  was packaged and 1.0 s in was not; with 3 s, 2.8 s in was and 3.0 s was not. The integration
  test `AStreamWhoseFirstKeyframeComesLateInTheSegmentIsStillPackaged` sends video 1.8 s behind
  its audio over SRT, and fails with the 1 s window. The
  price is the first segment only: on a real-time 2 s-keyframe feed it was listed 2.0 s after
  the first byte with 1 s of probe and 2.8 s with 3 s; later segments are not delayed. When
  ffmpeg still gives up for want of codec parameters, the packager says so in its log, with the
  window it had, since ffmpeg's own last line is only "Error opening output files". It is not
  restarted: the window already holds a keyframe interval, so a publisher it fails is one that
  breaks the keyframe contract, and a second ffmpeg would probe the same stream the same way.
- The packager needs the same host features as the worker (user namespaces, ADR-0032), and runs
  in the same kind of pod, plus a UDP port.
- A publisher that disconnects ends the stream (ADR-0047). A network blip between egress and
  packager therefore ends it too, unless SRT's own retransmission and idle timeout (5 s) ride it
  out, until the egress side is started into a new stream.
- The RTP or RTMP options of the brief are not available or not worth it, for the reasons
  above; this is the decision the brief's "record the choice in an ADR" asks for.

# 0059. Live playlists are served by the gateway from a single-flight cache, to any signed-in viewer

Status: Accepted
Date: 2026-09-29

## Context

The packager writes each stream's media playlist to `live/<stream>/index.m3u8` once per target
duration T, with the segments beside it (ADR-0047). Viewers play it as HLS from the store
(ADR-0014), which leaves two things to the gateway: who may fetch a stream's playlist, and how
to serve playlists that every viewer reloads about once per T (RFC 8216 6.3.4) without the store
paying for each reload. At 1,000 viewers and T = 2 s that is 500 playlist requests a second for
one stream. The segment URLs in the playlist must be signed (the bucket is private, ADR-0024),
and a playlist cannot be redirected to the store, for the reason ADR-0024 gives.

Nothing on the platform yet records a stream's owner or audience: the stream id is the only name
a stream has, the packager is started with it, and live chat is joined by it (ADR-0057). The
recording of a stream becomes an ordinary video owned by its broadcaster (M33).

## Options

Who may fetch a live playlist:

| Option | Why it was tempting | Verdict |
|---|---|---|
| The owner only, as for VOD (section 8.11) | The same rule everywhere | Rejected: a live stream exists to be watched by others, and no record says who its owner is |
| Anyone, without a token | Broadcasts are public; no JWT check per reload | Rejected: the gateway's routes all authenticate, the signed segment URLs would be handed to anyone on the internet, and an entitlement check later would change the contract |
| Any signed-in viewer, by stream id | What live chat already does (ADR-0057); tokens are checked the same way as for VOD; an entitlement hook can be added where the id is resolved | Accepted |

How a playlist is served:

| Option | Why it was tempting | Verdict |
|---|---|---|
| Fetch and rewrite per request, as VOD does | No new state | Rejected: one store read per viewer reload, 500 a second in the example, each a round trip on the offload pool |
| Viewers read the playlist from the store directly | No gateway work at all | Rejected: the playlist's relative URIs would resolve unsigned against a private bucket (ADR-0024) |
| An expiry-only cache | Simple | Rejected: when a copy expires, every request that arrives before the new read lands starts a read of its own; at 500 requests a second and a 50 ms read that is 25 reads per interval, not one |
| A cache of the rewritten playlist per stream, fresh for T/2, where a miss joins the read already in flight (single-flight), bounded by entries and bytes | One store read per stream per T/2 whatever the audience; the signing work is shared too | Accepted |
| Serve a stale copy while one read refreshes it | No request ever waits for the store | Rejected for now: a stale copy is up to T/2 older still, and the reader who waits costs one store round trip, tens of milliseconds, against a playlist reloaded every T |

## Decision

- **Route.** `GET /api/v1/live/{stream}/index.m3u8`, authenticated like every other API route
  (bearer token or the auth cookie). Any valid token may watch any stream. An id the packager
  could not have been started with (1 to 64 of `[A-Za-z0-9_-]`) and a stream with no playlist
  yet both answer `404`. The body is the stored playlist with every URI, `EXT-X-MAP`'s included,
  replaced by a URL signed for one hour, through the same rewriter as VOD (`core/util/hls`);
  `EXT-X-PROGRAM-DATE-TIME`, discontinuities and `EXT-X-ENDLIST` pass through unchanged. No
  view events are recorded for live playback: there is no video id to record them against
  until the recording exists.
- **Freshness.** A copy is fresh for half the playlist's own `EXT-X-TARGETDURATION`. The
  packager replaces the playlist once per T, and players reload it about once per T, so a copy
  at most T/2 old means a reload always finds the newer playlist once the store has it, and the
  store sees two reads per segment per gateway process however many viewers there are. A target
  duration outside what the packager writes (1 to 10 s) is refused as a broken playlist rather
  than cached for an interval nobody chose. An ended playlist (with `EXT-X-ENDLIST`) never
  changes again, since ADR-0047 refuses to restart an ended stream, and is kept for 60 s, like
  a VOD playlist.
- **Signing.** URLs are signed for one hour (`kLivePresignTtl`), which outlives every cached
  copy (at most 60 s, checked by a `static_assert`), a viewer's use of a window's segments, and
  a pause a viewer resumes from without reloading.
- **Single flight.** The cache lives on the reactor thread, one per gateway process. A miss
  starts one read on the offload pool, and every miss for the same stream that arrives while it
  runs waits on it. The read is taken out of the in-flight set before its waiters are answered,
  so a waiter that asks again (a pipelined request) gets the stored copy rather than joining a
  read that will not answer again.
- **Errors.** A missing playlist is remembered for 1 s, so viewers who arrive before the first
  segment cost one read a second between them. Every other failure (the store throttling or
  unreachable, a playlist that breaks a rewriting rule, a URL the store will not sign) is given
  to the requests waiting on that read and not kept: the next request reads again, and single
  flight already bounds a failing store to one read at a time per stream.
- **Bounds.** At most 512 streams and 4 MiB of rewritten playlists per process (a 10-segment
  window signs to about 7.5 KB, the longest window to about 85 KB), least recently used out
  first. A copy larger than the whole budget is served and not kept.
- **Headers.** `Cache-Control: private, no-cache` on a live playlist: the gateway's copy already
  lags the store by up to T/2, and the browser's own cache would add its age on top.
  `private, max-age=60` on an ended one, as on a VOD playlist.
- **Metrics.** `live_playlist_cache_hits_total`, `live_playlist_cache_misses_total`,
  `live_playlist_fetches_total`, `live_playlist_single_flight_joins_total`,
  `live_playlist_cache_evictions_total`, and the gauges `live_playlist_cache_entries` and
  `live_playlist_cache_bytes`; `playlist_requests_total{kind="live"}` counts the requests.

## Consequences

- Latency grows by up to T/2 over reading the store directly. Measured end to end
  (`tests/e2e/live.e2e.mjs`: the test publisher over SRT, live_packager, MinIO, this route, hls.js
  in Chrome for Testing 141, T = 2 s, a viewer joining six segments in), the picture's own clock
  read back from the screen gives a median glass-to-glass latency of 7.5, 7.7 and 7.7 s in three
  runs (each run's p95 within 0.1 s of its median, no stall), and `EXT-X-PROGRAM-DATE-TIME`
  gives 7.3 to 7.6 s, against about 7 s reading the store directly (ADR-0046). Over those runs
  the gateway answered 145 to 201 live playlist requests from two viewers with 25 to 42 store
  reads. The e2e workflow's live-playback job repeats the measurement on a hosted runner and
  puts the figures in the run's summary; its first run (ubuntu-24.04, run 36606280256) measured
  7.4 s median and p95 by the picture's clock, 7.2 s by program date time, no stall, and 43
  store reads for 200 playlist requests.
- The cache is per process: a deployment of N gateway replicas costs the store up to N reads per
  stream per T/2. With the gateway's replica counts that stays far inside R2's read rates.
- A viewer's playlist is never more than T/2 behind the store's, so the live edge a player
  computes moves in steps of up to half a segment; hls.js absorbs that without stalling (no stall
  in any run).
- Anyone with an account and a stream id can watch that stream. Reopen when streams gain
  audiences (followers-only, paid): the check belongs where `start_live` resolves the id, and the
  cache key stays the stream, with the entitlement decided before the cache is asked.
- The recording of a stream stays the owner's alone, as any video is; only the live window is
  open to every signed-in viewer.

// M31 acceptance: a stream the packager cuts from a live publisher plays in hls.js, its
// playlist through the gateway's live route and its segments from the store through the URLs
// the gateway signed; the manifest's media sequence only ever grows; and when the publisher
// stops the packager ends the playlist and the player finishes cleanly. Glass-to-glass latency
// is measured on the way, printed, and written to $ULW_LIVE_LATENCY_FILE when that is set.
import { writeFileSync } from 'node:fs';

import { expect, test } from '@playwright/test';

import { startLiveStack } from './live-stack.mjs';

// Long enough to measure over: the viewer joins after six segments, and the tail that plays
// out after the publisher stops is another three.
const kPublishSeconds = 50;
const kSegmentSeconds = 2;
const kSegmentsBeforeJoin = 6;

const median = (values) => values.toSorted((a, b) => a - b)[Math.floor(values.length / 2)];
const percentile = (values, p) =>
  values.toSorted((a, b) => a - b)[Math.min(values.length - 1, Math.floor(values.length * p))];

// The value of an unlabelled sample, or of one with exactly `labels`, in the gateway's metrics.
const metric = (text, name, labels = '') => {
  const line = text.split('\n').find((l) => l.startsWith(`${name}${labels} `));
  return line === undefined ? NaN : Number(line.slice(line.lastIndexOf(' ') + 1));
};

test('a live stream plays, keeps a monotonic manifest, and ends cleanly after ENDLIST',
  async ({ page }, testInfo) => {
    const stack = await startLiveStack({ seconds: kPublishSeconds, segment: kSegmentSeconds });
    const viewer = { headers: { authorization: `Bearer ${stack.token}` } };
    try {
      // Watches the manifest itself, as a second viewer's player would, apart from the page.
      const sequences = [];
      let sawEndlist = false;
      const poll = setInterval(async () => {
        try {
          const text = await (await fetch(stack.gatewayManifest, viewer)).text();
          const sequence = text.match(/#EXT-X-MEDIA-SEQUENCE:(\d+)/);
          if (sequence) {
            sequences.push(Number(sequence[1]));
          }
          sawEndlist = sawEndlist || text.includes('#EXT-X-ENDLIST');
        } catch {
          // Not there yet, or the run is ending.
        }
      }, 500);

      const requests = [];
      page.on('request', (r) => requests.push(r.url()));
      const publisher = stack.publish();
      // A viewer joins a stream that has been running for a while, which is what live is:
      // hls.js then starts three target durations behind the newest segment, where a viewer
      // who arrives with the first segment starts at the beginning and sits closer to it.
      await expect.poll(async () => {
        const response = await fetch(stack.gatewayManifest, viewer);
        return response.ok ? (await response.text()).split('#EXTINF').length - 1 : 0;
      }, { timeout: 60_000, intervals: [200] }).toBeGreaterThanOrEqual(kSegmentsBeforeJoin);
      // Same origin as the page, as behind the Askedin route: the cookie rides along on every
      // playlist request, and never on a segment's, which goes to the store.
      await page.context().addCookies([{ name: 'auth_token', value: stack.token,
        url: stack.origin }]);
      await page.goto(`${stack.origin}/?src=${encodeURIComponent(stack.manifest)}`);
      await page.waitForFunction(() => window.player.state.started, null, { timeout: 60_000 });
      const startedAfter = await page.evaluate(() => window.player.state.startedAfterMs);

      await publisher.exited;
      await page.waitForFunction(() => window.player.state.ended, null, { timeout: 60_000 });
      clearInterval(poll);
      expect(await stack.packager.exited).toBe(0);
      const state = await page.evaluate(() => window.player.state);
      const metrics = await (await fetch(`${stack.gateway}/metrics`)).text();

      // The measurement: what is on screen against when it was made, from the third second
      // of playback on, when the first buffered frames are behind us.
      const samples = state.latencies.filter((s) => s.position > 3);
      expect(samples.length).toBeGreaterThan(50);
      const strip = samples.map((s) => s.stripMs);
      const dated = samples.filter((s) => s.pdtMs !== null).map((s) => s.pdtMs);
      const summary = {
        publishSeconds: kPublishSeconds,
        segmentSeconds: kSegmentSeconds,
        playbackStartedAfterMs: startedAfter,
        samples: samples.length,
        glassToGlassMs: { median: median(strip), p95: percentile(strip, 0.95),
          min: Math.min(...strip), max: Math.max(...strip) },
        programDateTimeMs: dated.length === 0 ? null : { median: median(dated),
          p95: percentile(dated, 0.95), samples: dated.length },
        stalls: state.stalls,
        waitingAfterStart: state.waitingAfterStart,
        playlistLoads: state.playlists.length,
        gateway: {
          livePlaylistRequests: metric(metrics, 'playlist_requests_total', '{kind="live"}'),
          storeReads: metric(metrics, 'live_playlist_fetches_total'),
          cacheHits: metric(metrics, 'live_playlist_cache_hits_total'),
          singleFlightJoins: metric(metrics, 'live_playlist_single_flight_joins_total'),
        },
      };
      console.log(`live latency: ${JSON.stringify(summary)}`);
      await testInfo.attach('live-latency.json',
        { body: JSON.stringify(summary, null, 2), contentType: 'application/json' });
      if (process.env.ULW_LIVE_LATENCY_FILE) {
        writeFileSync(process.env.ULW_LIVE_LATENCY_FILE, `${JSON.stringify(summary, null, 2)}\n`);
      }

      // A viewer sits three target durations behind the newest segment (RFC 8216 6.3.3), which
      // is itself up to one segment old when it is listed; the gateway's copy of the playlist
      // is up to half a segment older than the store's (ADR-0059); and SRT's latency, the
      // upload and the fetches take a moment, 2 s allowed: 3T + T + T/2 + 2 s. At T = 2 that
      // is 11 s, against about 7 to 8 s measured, so a regression of a segment or more is
      // caught.
      expect(summary.glassToGlassMs.median).toBeGreaterThan(3 * kSegmentSeconds * 1000);
      expect(summary.glassToGlassMs.median).toBeLessThan((4.5 * kSegmentSeconds + 2) * 1000);

      // hls.js saw the window slide: it never reloaded a playlist that went backwards.
      const starts = state.playlists.map((p) => p.startSN);
      expect(starts.length).toBeGreaterThan(10);
      expect(starts).toEqual(starts.toSorted((a, b) => a - b));
      expect(starts.at(-1)).toBeGreaterThan(starts[0]);
      // And so did an observer of the store, over the whole run.
      expect(sequences.length).toBeGreaterThan(10);
      expect(sequences).toEqual(sequences.toSorted((a, b) => a - b));

      // The end: ENDLIST in the store, a playlist hls.js took as final, playback finished
      // without an error of any kind.
      expect(sawEndlist).toBe(true);
      expect(state.playlists.at(-1).live).toBe(false);
      expect(state.ended).toBe(true);
      expect(state.errors).toEqual([]);

      // Playlists from the gateway, segments from the store: every segment came from the
      // bucket, on a URL the gateway signed, and no playlist from anywhere but the route.
      const media = requests.filter((u) => /\.(m4s|mp4)(\?|$)/.test(new URL(u).pathname));
      expect(media.length).toBeGreaterThan(10);
      expect(new Set(media.map((u) => new URL(u).origin))).toEqual(new Set([stack.storageOrigin]));
      expect(media.every((u) => new URL(u).searchParams.has('X-Amz-Signature'))).toBe(true);
      const playlists = requests.filter((u) => new URL(u).pathname.endsWith('.m3u8'));
      expect(new Set(playlists.map((u) => new URL(u).origin))).toEqual(new Set([stack.origin]));
      expect(stack.proxied.filter((p) => p === `/api/v1/live/${stack.id}/index.m3u8`).length)
        .toBe(playlists.length);
      // The bucket itself answers nobody who has no signature.
      const unsigned = await fetch(new URL(media[0]).origin + new URL(media[0]).pathname);
      expect(unsigned.status).toBe(403);

      // Two viewers polling every 0.5 s and every T cost the store at most one read per half
      // segment, however many requests they made: the cache answered the rest.
      const g = summary.gateway;
      expect(g.cacheHits + g.singleFlightJoins).toBeGreaterThan(0);
      expect(g.storeReads).toBeLessThan(g.livePlaylistRequests);
      expect(g.storeReads).toBeLessThanOrEqual(
        Math.ceil((kPublishSeconds + 30) / (kSegmentSeconds / 2)));
    } finally {
      await stack.stop();
    }
  });

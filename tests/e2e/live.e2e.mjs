// M31 acceptance: a stream the packager cuts from a live publisher plays in hls.js straight
// from the store; the manifest's media sequence only ever grows; and when the publisher stops
// the packager ends the playlist and the player finishes cleanly. Glass-to-glass latency is
// measured on the way and printed.
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

test('a live stream plays, keeps a monotonic manifest, and ends cleanly after ENDLIST',
  async ({ page }, testInfo) => {
    const stack = await startLiveStack({ seconds: kPublishSeconds, segment: kSegmentSeconds });
    try {
      // Watches the manifest itself, as any viewer's player would, apart from the page.
      const sequences = [];
      let sawEndlist = false;
      const poll = setInterval(async () => {
        try {
          const text = await (await fetch(stack.manifest, { cache: 'no-store' })).text();
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
        const response = await fetch(stack.manifest);
        return response.ok ? (await response.text()).split('#EXTINF').length - 1 : 0;
      }, { timeout: 60_000, intervals: [200] }).toBeGreaterThanOrEqual(kSegmentsBeforeJoin);
      await page.goto(`${stack.origin}/?src=${encodeURIComponent(stack.manifest)}`);
      await page.waitForFunction(() => window.player.state.started, null, { timeout: 60_000 });
      const startedAfter = await page.evaluate(() => window.player.state.startedAfterMs);

      await publisher.exited;
      await page.waitForFunction(() => window.player.state.ended, null, { timeout: 60_000 });
      clearInterval(poll);
      expect(await stack.packager.exited).toBe(0);
      const state = await page.evaluate(() => window.player.state);

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
      };
      console.log(`live latency: ${JSON.stringify(summary)}`);
      await testInfo.attach('live-latency.json',
        { body: JSON.stringify(summary, null, 2), contentType: 'application/json' });

      // A viewer sits three target durations behind the newest segment (RFC 8216 6.3.3), which
      // is itself up to one segment old when it is listed, and fetching and uploading it take a
      // moment: 3T + T, plus 2 s for SRT's latency, the upload and the fetch. At T = 2 that is
      // 10 s, against about 7 s measured, so a regression of a segment or more is caught.
      expect(summary.glassToGlassMs.median).toBeGreaterThan(3 * kSegmentSeconds * 1000);
      expect(summary.glassToGlassMs.median).toBeLessThan((4 * kSegmentSeconds + 2) * 1000);

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

      // Nothing but the page and the store: every segment came from the bucket.
      const media = requests.filter((u) => /\.(m4s|mp4)(\?|$)/.test(new URL(u).pathname));
      expect(media.length).toBeGreaterThan(10);
      expect(new Set(media.map((u) => new URL(u).origin))).toEqual(new Set([stack.storageOrigin]));
    } finally {
      await stack.stop();
    }
  });

// ADR-0092 acceptance: live publishing as a real client does it, with nothing but the gateway's
// stream service issuing tickets. A broadcaster's page starts a stream through the API and
// publishes a camera and microphone (Chromium's fake devices) over WHIP with the ticket it got;
// the stream service starts the packager and relays the publisher to it; a second user watches
// the live playlist in hls.js; the broadcaster ends the stream; the playlist ends, and the
// recording becomes a video that only the broadcaster can see and that plays.
import { mkdirSync, writeFileSync } from 'node:fs';
import path from 'node:path';

import { expect, test } from '@playwright/test';

import { config, startStack } from './stack.mjs';

const bin = (rel) => path.join(config.build, rel);
const kBroadcaster = 'e2e-broadcaster';
// How long the stream runs before its owner ends it, counted in segments the viewer's player
// takes after it starts: 2 s each.
const kWatchedSegments = 6;
const kSegmentMs = 2_000;

// A UDP port nothing holds now, for the packager's SRT listener.
async function freeUdpPort() {
  const { createSocket } = await import('node:dgram');
  const socket = createSocket('udp4');
  await new Promise((resolve) => socket.bind(0, '127.0.0.1', resolve));
  const { port } = socket.address();
  await new Promise((resolve) => socket.close(resolve));
  return port;
}

const json = async (response) => ({ status: response.status, body: await response.json() });

test('a stream started through the API is published, watched, ended and recorded',
  async ({ browser }, testInfo) => {
    const srtPort = await freeUdpPort();
    const stack = await startStack({
      player: 'live-player.html',
      bucket: process.env.ULW_LIVE_PUBLISH_E2E_BUCKET ?? 'ulw-live-publish-e2e',
      users: [kBroadcaster],
      gatewayEnv: ({ work }) => {
        const scratch = path.join(work, 'live');
        mkdirSync(scratch, { mode: 0o700 });
        return {
          LIVEKIT_API_URL: process.env.LIVEKIT_API_URL ?? 'http://127.0.0.1:7880',
          LIVEKIT_CLIENT_URL: process.env.LIVEKIT_CLIENT_URL ?? 'ws://127.0.0.1:7880',
          LIVEKIT_API_KEY: process.env.LIVEKIT_API_KEY ?? 'ulw-dev-key',
          LIVEKIT_API_SECRET: process.env.LIVEKIT_API_SECRET ??
            'ulw-dev-secret-testtest123-not-a-real-secret',
          ULW_LIVE_PACKAGER: 'process',
          ULW_LIVE_PACKAGER_BIN: bin('apps/live-packager/live_packager'),
          ULW_LIVE_PACKAGER_SCRATCH_DIR: scratch,
          ULW_LIVE_PACKAGER_PORT: String(srtPort),
          ULW_LIVE_PACKAGER_SRT: `srt://127.0.0.1:${srtPort}`,
          ULW_LIVE_MAX_STREAMS: '1',
        };
      },
    });
    const owner = { authorization: `Bearer ${stack.tokens[kBroadcaster]}` };
    const viewer = { authorization: `Bearer ${stack.token}` };
    const api = (method, route, headers) => fetch(`${stack.gateway}${route}`, { method, headers });
    const context = await browser.newContext();
    try {
      // The broadcaster's page: the documented flow, from create to live.
      const publisherPage = await context.newPage();
      await publisherPage.goto(`${stack.origin}/publisher`);
      const published = await publisherPage.evaluate((token) => window.publish(token),
        stack.tokens[kBroadcaster]);
      testInfo.attach('publish.json', { body: JSON.stringify(published, null, 2),
        contentType: 'application/json' });
      expect(published.whip).toBe(201);
      expect(published.patched).toBe(204);
      expect(published.start).toBe(200);
      expect(published.live.state).toBe('live');
      const { id } = published;
      const route = `/api/v1/live/${id}`;

      // Anyone signed in sees it live and where to watch; only the owner may act on it.
      const seen = await json(await api('GET', route, viewer));
      expect(seen.status).toBe(200);
      expect(seen.body.state).toBe('live');
      expect(seen.body.playlist).toBe(`${route}/index.m3u8`);
      expect(seen.body).not.toHaveProperty('video_id');
      expect((await api('POST', `${route}/ticket`, viewer)).status).toBe(404);
      expect((await api('POST', `${route}/end`, viewer)).status).toBe(404);
      // The owner's second stream is this one.
      const again = await json(await api('POST', '/api/v1/live', owner));
      expect(again.status).toBe(200);
      expect(again.body.id).toBe(id);

      // A second user watches the live playlist through the gateway.
      await expect.poll(async () => {
        const playlist = await api('GET', `${route}/index.m3u8`, viewer);
        return playlist.ok ? (await playlist.text()).split('#EXTINF').length - 1 : 0;
      }, { timeout: 60_000, intervals: [500] }).toBeGreaterThanOrEqual(3);
      const viewerPage = await context.newPage();
      await context.addCookies([{ name: 'auth_token', value: stack.token, url: stack.origin }]);
      await viewerPage.goto(`${stack.origin}/?src=${encodeURIComponent(
        `${stack.origin}${route}/index.m3u8`)}`);
      await viewerPage.waitForFunction(() => window.player.state.started, null,
        { timeout: 60_000 });
      const startSN = await viewerPage.evaluate(() => window.player.state.playlists.at(-1).endSN);
      await viewerPage.waitForFunction((from) => window.player.state.playlists.at(-1).endSN >=
        from, startSN + kWatchedSegments, { timeout: 60_000 });

      // The owner ends it: the playlist ends, and the player plays out what is listed.
      const ended = await publisherPage.evaluate(() => window.endStream());
      expect(ended.status).toBe(200);
      expect(ended.body.state).toBe('ended');
      expect(ended.body.ended_by).toBe('owner');
      await viewerPage.waitForFunction(() => window.player.state.ended, null, { timeout: 60_000 });
      const played = await viewerPage.evaluate(() => window.player.state);
      expect(played.errors).toEqual([]);
      expect(played.playlists.at(-1).live).toBe(false);
      expect((await api('POST', `${route}/ticket`, owner)).status).toBe(409);

      // The recording: a video of the owner's, which goes on to play.
      let video;
      await expect.poll(async () => {
        video = (await json(await api('GET', route, owner))).body.video_id;
        return video;
      }, { timeout: 120_000, intervals: [1000] }).toEqual(expect.any(String));
      expect((await json(await api('GET', route, viewer))).body).not.toHaveProperty('video_id');
      expect((await api('GET', `/api/v1/videos/${video}`, viewer)).status).toBe(404);
      await expect.poll(async () => (await json(await api('GET', `/api/v1/videos/${video}`,
        owner))).body.state, { timeout: 180_000, intervals: [1000] }).toBe('ready');
      const recorded = await json(await api('GET', `/api/v1/videos/${video}`, owner));
      expect(recorded.body.title).toBe(`Live stream ${id}`);
      expect(recorded.body.visibility).toBe('private');
      expect(recorded.body.duration_ms).toBeGreaterThan(kWatchedSegments * kSegmentMs);
      // Its renditions are the VOD ladder for the stream's 720p: 720p and 360p, each a media
      // playlist through the gateway with segments in it.
      const master = await api('GET', `/api/v1/videos/${video}/master.m3u8`, owner);
      expect(master.status).toBe(200);
      const variants = (await master.text()).split('\n').filter((l) => l && !l.startsWith('#'));
      const renditions = variants.map((uri) => new URL(uri, stack.gateway).pathname);
      expect(renditions.toSorted()).toEqual([`/api/v1/videos/${video}/360p/index.m3u8`,
        `/api/v1/videos/${video}/720p/index.m3u8`]);
      for (const rendition of renditions) {
        const media = await api('GET', rendition, owner);
        expect(media.status).toBe(200);
        const text = await media.text();
        expect(text).toContain('#EXT-X-ENDLIST');
        expect(text.split('#EXTINF').length - 1).toBeGreaterThan(0);
      }

      // A stream that ends with no media becomes no video: the owner's next stream, ended
      // before anything was published, keeps a null video_id.
      const empty = await json(await api('POST', '/api/v1/live', owner));
      expect(empty.status).toBe(201);
      const emptyRoute = `/api/v1/live/${empty.body.id}`;
      const emptyEnded = await json(await api('POST', `${emptyRoute}/end`, owner));
      expect(emptyEnded.status).toBe(200);
      expect(emptyEnded.body.state).toBe('ended');
      expect(emptyEnded.body.video_id).toBeNull();
      // Long enough for a recording to have been queued, had there been one.
      await new Promise((resolve) => setTimeout(resolve, 5_000));
      const emptyLater = await json(await api('GET', emptyRoute, owner));
      expect(emptyLater.body.state).toBe('ended');
      expect(emptyLater.body.video_id).toBeNull();
      console.log(`live publish: ${JSON.stringify({ id, video,
        durationMs: recorded.body.duration_ms, viewerStartedAfterMs: played.startedAfterMs,
        playlistLoads: played.playlists.length })}`);
    } catch (e) {
      // The gateway's log holds its packagers' too: they write to its stdout.
      writeFileSync(testInfo.outputPath('gateway.log'), stack.gatewayOutput());
      writeFileSync(testInfo.outputPath('worker.log'), stack.workerOutput());
      throw e;
    } finally {
      await context.close();
      await stack.stop();
    }
  });

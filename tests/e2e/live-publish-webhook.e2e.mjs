// ADR-0093 acceptance: a stream whose client never calls start or end, as an encoder with a fixed
// token, or a client that crashes. The broadcaster's page creates a stream and publishes over
// WHIP, and nothing more: LiveKit's webhooks to the gateway's own listener take it live. The
// page then closes its WHIP session (a DELETE, as RFC 9725 has a client stop) and never calls
// the gateway's end: the stream ends with nobody asking it to, and its recording becomes a video
// that plays.
//
// Two ends race there, and both are right. LiveKit reports the publisher gone at once, and after
// the grace the gateway, having asked LiveKit, ends the row `publisher_left`. LiveKit's recorder
// ends with the publisher, the packager ends the playlist and exits after recording, and the
// sweep then ends the row `finished`, unless the first got there. The run takes either, and
// checks that the webhooks saw the departure; tests/integration/live_webhook_test.cpp ends a
// stream `publisher_left` against the same LiveKit with nothing to race it.
//
// A client that vanishes without the DELETE is not this run: LiveKit notices it only by its own
// timeout, while the packager, sent nothing, ends the playlist 5 segments (10 s) in and exits
// counting the stall a failure, so the sweep ends the row `failed` first (ADR-0093).
//
// LiveKit must post its webhooks to this gateway: deploy/local/compose.yaml's LiveKit posts to
// 127.0.0.1:7890, the port used here unless ULW_E2E_WEBHOOK_PORT names another.
import { mkdirSync, writeFileSync } from 'node:fs';
import path from 'node:path';

import { expect, test } from '@playwright/test';

import { config, startStack } from './stack.mjs';

const bin = (rel) => path.join(config.build, rel);
const kBroadcaster = 'e2e-hooked-broadcaster';
const kSegments = 4;
const kSegmentMs = 2_000;
// The shortest grace: a reconnect is not what this run is about.
const kGraceSeconds = 1;

async function freeUdpPort() {
  const { createSocket } = await import('node:dgram');
  const socket = createSocket('udp4');
  await new Promise((resolve) => socket.bind(0, '127.0.0.1', resolve));
  const { port } = socket.address();
  await new Promise((resolve) => socket.close(resolve));
  return port;
}

const json = async (response) => ({ status: response.status, body: await response.json() });

// A counter's value in the gateway's /metrics, 0 while absent.
function metric(text, name) {
  const line = text.split('\n').find((l) => l.startsWith(`${name} `));
  return line ? Number(line.split(' ')[1]) : 0;
}

test('a stream published over WHIP goes live and ends by the media server\'s word alone',
  async ({ browser }, testInfo) => {
    const srtPort = await freeUdpPort();
    const webhookPort = process.env.ULW_E2E_WEBHOOK_PORT ?? '7890';
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
          ULW_LIVE_WEBHOOK_PORT: webhookPort,
          ULW_LIVE_PUBLISHER_GRACE_SECONDS: String(kGraceSeconds),
        };
      },
    });
    const owner = { authorization: `Bearer ${stack.tokens[kBroadcaster]}` };
    const api = (method, route, headers) => fetch(`${stack.gateway}${route}`, { method, headers });
    const metrics = async () => (await fetch(`${stack.gateway}/metrics`)).text();
    const context = await browser.newContext();
    try {
      const publisherPage = await context.newPage();
      await publisherPage.goto(`${stack.origin}/publisher`);
      const published = await publisherPage.evaluate(
        (token) => window.publish(token, { start: false }), stack.tokens[kBroadcaster]);
      testInfo.attach('publish.json', { body: JSON.stringify(published, null, 2),
        contentType: 'application/json' });
      expect(published.whip).toBe(201);
      expect(published.patched).toBe(204);
      expect(published).not.toHaveProperty('start');
      const { id } = published;
      const route = `/api/v1/live/${id}`;

      // Nobody called start: the publisher's own tracks took it live.
      await expect.poll(async () => (await json(await api('GET', route, owner))).body.state,
        { timeout: 60_000, intervals: [500] }).toBe('live');
      const seen = await metrics();
      expect(metric(seen, 'live_webhooks_total{outcome="accepted"}')).toBeGreaterThan(0);
      expect(metric(seen, 'live_webhooks_total{outcome="refused"}')).toBe(0);
      expect(metric(seen, 'live_webhook_starts_total')).toBeGreaterThan(0);

      // It is watchable: segments arrive on the playlist.
      await expect.poll(async () => {
        const playlist = await api('GET', `${route}/index.m3u8`, owner);
        return playlist.ok ? (await playlist.text()).split('#EXTINF').length - 1 : 0;
      }, { timeout: 60_000, intervals: [500] }).toBeGreaterThanOrEqual(kSegments);

      // The client closes its session, and nothing else. Only the playlist and /metrics are read
      // until the end is in, so no status request ends the row on the playlist's word.
      expect([200, 204]).toContain(await publisherPage.evaluate(() => window.closePublisher()));
      await expect.poll(async () => {
        const playlist = await api('GET', `${route}/index.m3u8`, owner);
        return playlist.ok && (await playlist.text()).includes('#EXT-X-ENDLIST');
      }, { timeout: 90_000, intervals: [500] }).toBe(true);
      await expect.poll(async () => {
        const text = await metrics();
        return metric(text, 'live_publisher_departures_total') >= 1 &&
          metric(text, 'live_streams_ended_total{reason="publisher_left"}') +
          metric(text, 'live_streams_ended_total{reason="finished"}') === 1;
      }, { timeout: 90_000, intervals: [250] }).toBe(true);
      const ended = await json(await api('GET', route, owner));
      expect(ended.body.state).toBe('ended');
      expect(['publisher_left', 'finished']).toContain(ended.body.ended_by);
      const endedText = await metrics();
      expect(metric(endedText, 'live_streams_ended_total{reason="owner"}')).toBe(0);
      expect(metric(endedText, 'live_streams_ended_total{reason="failed"}')).toBe(0);
      expect(metric(endedText, 'live_streams_ended_total{reason="timeout"}')).toBe(0);
      expect(ended.body.ended_at).toBeGreaterThanOrEqual(ended.body.live_at);
      expect((await api('POST', `${route}/ticket`, owner)).status).toBe(409);
      const after = await metrics();
      expect(metric(after, 'live_publisher_departures_total')).toBeGreaterThan(0);
      expect(metric(after, 'live_webhooks_total{outcome="refused"}')).toBe(0);

      // The recording: the owner's video, which goes on to play.
      let video;
      await expect.poll(async () => {
        video = (await json(await api('GET', route, owner))).body.video_id;
        return video;
      }, { timeout: 120_000, intervals: [1000] }).toEqual(expect.any(String));
      await expect.poll(async () => (await json(await api('GET', `/api/v1/videos/${video}`,
        owner))).body.state, { timeout: 180_000, intervals: [1000] }).toBe('ready');
      const recorded = await json(await api('GET', `/api/v1/videos/${video}`, owner));
      expect(recorded.body.title).toBe(`Live stream ${id}`);
      expect(recorded.body.duration_ms).toBeGreaterThan(kSegments * kSegmentMs);
      expect((await api('GET', `/api/v1/videos/${video}/master.m3u8`, owner)).status).toBe(200);
      console.log(`live publish (webhooks): ${JSON.stringify({ id, video,
        endedBy: ended.body.ended_by, durationMs: recorded.body.duration_ms, endedAfterLiveS: ended.body.ended_at -
          ended.body.live_at })}`);
    } catch (e) {
      writeFileSync(testInfo.outputPath('gateway.log'), stack.gatewayOutput());
      writeFileSync(testInfo.outputPath('worker.log'), stack.workerOutput());
      throw e;
    } finally {
      await context.close();
      await stack.stop();
    }
  });

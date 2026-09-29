// M11 acceptance: a clip uploaded through the gateway, transcoded by the worker, plays to its
// end in hls.js; a rendition switch forced mid-playback stalls nothing; and every segment comes
// from the object store, none through the gateway.
import { expect, test } from '@playwright/test';
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import path from 'node:path';

import { startStack } from './stack.mjs';

// Four segments of 4 s: room to switch after the first and still play two on the new rung.
const kSeconds = 16;

let stack;

test.beforeAll(async () => {
  stack = await startStack();
});

test.afterAll(async () => {
  await stack?.stop();
});

function makeClip(file) {
  execFileSync('ffmpeg', ['-nostdin', '-v', 'error', '-y',
    '-f', 'lavfi', '-i', 'testsrc2=size=1280x720:rate=30',
    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
    '-t', String(kSeconds), '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p',
    '-c:a', 'aac', '-shortest', file]);
}

async function api(method, pathname, { body, headers = {} } = {}) {
  return fetch(`${stack.gateway}${pathname}`, {
    method,
    body,
    headers: { authorization: `Bearer ${stack.token}`, ...headers },
  });
}

// The resumable upload protocol, as a client would drive it: create, append each chunk at the
// offset the gateway reports, commit.
async function upload(file) {
  const bytes = readFileSync(file);
  const created = await api('POST', '/api/v1/uploads', {
    body: JSON.stringify({ filename: 'e2e.mp4', size_bytes: bytes.length,
      content_type: 'video/mp4' }),
    headers: { 'content-type': 'application/json' },
  });
  expect(created.status).toBe(201);
  const { video_id: video, upload_id: id, chunk_size: chunk } = await created.json();
  let offset = 0;
  while (offset < bytes.length) {
    const piece = bytes.subarray(offset, offset + chunk);
    const r = await api('PATCH', `/api/v1/uploads/${id}`,
      { body: piece, headers: { 'upload-offset': String(offset) } });
    expect(r.status).toBe(204);
    offset = Number(r.headers.get('upload-offset'));
  }
  expect((await api('POST', `/api/v1/uploads/${id}/commit`)).status).toBe(200);
  return video;
}

test('an upload plays to its end, switches rendition without a stall, and never streams ' +
  'through the gateway', async ({ page, context }) => {
  const clip = path.join(stack.work, 'clip.mp4');
  makeClip(clip);
  const video = await upload(clip);
  await expect.poll(async () => (await (await api('GET', `/api/v1/videos/${video}`)).json()).state,
    { timeout: 240_000, intervals: [500] }).toBe('ready');

  const requests = [];
  page.on('request', (r) => requests.push(r.url()));
  // Same origin as the page, as behind the Askedin route: the cookie rides along on playlist
  // requests and never goes to the bucket.
  await context.addCookies([{ name: 'auth_token', value: stack.token, url: stack.origin }]);
  await page.goto(`${stack.origin}/?video=${video}`);

  await page.waitForFunction(() => window.player.state.started &&
    document.getElementById('video').currentTime > 2);
  const top = await page.evaluate(() => {
    const { hls } = window.player;
    hls.nextLevel = hls.levels.length - 1;
    return hls.levels.length - 1;
  });
  await page.waitForFunction((level) => window.player.state.switchedTo === level &&
    window.player.state.playedLevels.includes(level), top);
  await page.waitForFunction(() => window.player.state.ended, null,
    { timeout: (kSeconds + 30) * 1000 });

  const state = await page.evaluate(() => window.player.state);
  const duration = await page.evaluate(() => document.getElementById('video').duration);
  console.log(`levels ${JSON.stringify(state.levels)}, played ${JSON.stringify(state.playedLevels)}` +
    `, stalls ${state.stalls.length}, waiting after start ${state.waitingAfterStart}` +
    `, duration ${duration.toFixed(2)} s`);
  expect(state.levels).toEqual([360, 720]);
  console.log(`hls.js errors: ${JSON.stringify(state.errors)}`);
  // hls.js 1.7 reports, without acting on it, a fragment of the new rung appended over media
  // the old one had already buffered: the smooth switch replacing what it keeps, not a fault.
  // Anything else, and anything fatal, fails the run.
  expect(state.errors.filter((e) => e.fatal ||
    !e.what.startsWith(`mediaError/bufferAppendNoProgress level ${top} `))).toEqual([]);
  expect(state.stalls).toEqual([]);
  expect(state.waitingAfterStart).toBe(0);
  expect(state.playedLevels[0]).toBe(0);
  expect(state.playedLevels.at(-1)).toBe(top);
  expect(duration).toBeCloseTo(kSeconds, 0);

  // Every init and media segment, of both rungs, came from the bucket.
  const media = requests.filter((u) => /\.(m4s|mp4)(\?|$)/.test(new URL(u).pathname));
  const hosts = new Set(media.map((u) => new URL(u).origin));
  console.log(`${media.length} segment requests, from ${[...hosts].join(', ')}; ` +
    `${stack.proxied.length} requests passed to the gateway, all playlists`);
  expect([...hosts]).toEqual([stack.storageOrigin]);
  expect(media.some((u) => u.includes('/360p/'))).toBe(true);
  expect(media.some((u) => u.includes('/720p/'))).toBe(true);
  expect(media.every((u) => u.includes('X-Amz-Signature='))).toBe(true);
  // What reached the gateway was playlists and nothing else.
  expect(stack.proxied.length).toBeGreaterThanOrEqual(3);
  expect(stack.proxied.every((p) => p.endsWith('.m3u8'))).toBe(true);
  const gatewayPort = `:${stack.gatewayPort}`;
  expect(requests.filter((u) => new URL(u).host.endsWith(gatewayPort))).toEqual([]);
  expect(requests.every((u) => [stack.origin, stack.storageOrigin]
    .includes(new URL(u).origin))).toBe(true);
});

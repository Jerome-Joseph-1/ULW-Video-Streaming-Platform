// M30 acceptance: live ingest over WHIP (RFC 9725), straight to the SFU with a publisher ticket
// (ADR-0056). A GStreamer whipsink publisher is exactly one producer; its DELETE ends the session
// at once; a candidate trickled after the answer is what the connection is built on; and a
// published stream reaches the M31 packager through the SFU's recorder and becomes a live HLS
// playlist that ends with the session.
import { expect, test } from '@playwright/test';
import { execFileSync, spawn } from 'node:child_process';
import { createHmac, randomBytes, randomUUID } from 'node:crypto';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';

import { launchPeer, servePage, startSignalling } from './peers.mjs';

const here = path.dirname(new URL(import.meta.url).pathname);
const build = path.dirname(path.dirname(process.env.ULW_CALL_HARNESS ?? ''));

// LiveKit drops a participant that went silent without a DELETE only after 10 s without ICE
// traffic, 5 s more and a 5 s cleanup (20 s, ADR-0050), and a WHIP session that stops sending
// lingered 49 s in a trial run. A DELETE takes one request and the room's update, milliseconds
// on loopback; 5 s tells the two apart with room for a loaded machine.
const kDeleteBoundMs = 5_000;
// The packager's segment length, which the recorder's keyframe interval must equal (ADR-0046).
const kSegmentSeconds = 2;

function metricsFile(name, metrics) {
  mkdirSync(path.join(here, 'test-results'), { recursive: true });
  writeFileSync(path.join(here, 'test-results', `ingest-${name}.json`),
    JSON.stringify(metrics, null, 2));
  console.log(JSON.stringify(metrics));
}

// RoomService.ListParticipants, with a token of its own: the port has no use for it, the test
// needs to see what the SFU sees.
async function participants(roomName) {
  const now = Math.floor(Date.now() / 1000);
  const encode = (value) => Buffer.from(JSON.stringify(value)).toString('base64url');
  const unsigned = `${encode({ alg: 'HS256', typ: 'JWT' })}.${encode({
    iss: process.env.LIVEKIT_API_KEY, nbf: now, exp: now + 60,
    video: { roomAdmin: true, room: roomName },
  })}`;
  const signature = createHmac('sha256', process.env.LIVEKIT_API_SECRET).update(unsigned)
    .digest('base64url');
  const response = await fetch(
    `${process.env.LIVEKIT_API_URL}/twirp/livekit.RoomService/ListParticipants`, {
      method: 'POST',
      headers: { 'content-type': 'application/json', authorization: `Bearer ${unsigned}.${signature}` },
      body: JSON.stringify({ room: roomName }),
    });
  expect(response.status).toBe(200);
  return (await response.json()).participants ?? [];
}

// Participants that send media: a WHIP publisher's tracks, not the recorder, which only
// subscribes.
async function producers(roomName) {
  return (await participants(roomName))
    .filter((p) => (p.tracks ?? []).length > 0)
    .map((p) => ({ identity: p.identity, sid: p.sid, kinds: p.tracks.map((t) => t.type).sort() }));
}

const whipsinkDir = (() => {
  let dir;
  return () => {
    dir ??= execFileSync(path.join(here, 'fetch-whipsink.sh'), { encoding: 'utf8' }).trim();
    return dir;
  };
})();

// gst-launch publishing a test picture and tone with whipsink, as an encoder would: VP8 and
// Opus, the codecs every WebRTC SFU takes. On SIGINT gst-launch takes the pipeline down, and
// whipsink's teardown sends the DELETE.
function whipsink(ticket) {
  const child = spawn('gst-launch-1.0', [
    'videotestsrc', 'is-live=true', '!', 'video/x-raw,width=640,height=360,framerate=30/1', '!',
    'videoconvert', '!', 'vp8enc', 'deadline=1', `keyframe-max-dist=${30 * kSegmentSeconds}`, '!',
    'rtpvp8pay', '!', 'application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000',
    '!', 'whip.sink_0',
    'audiotestsrc', 'is-live=true', '!', 'audioconvert', '!', 'audioresample', '!', 'opusenc', '!',
    'rtpopuspay', '!', 'application/x-rtp,media=audio,encoding-name=OPUS,payload=111,clock-rate=48000',
    '!', 'whip.sink_1',
    'whipsink', 'name=whip', `whip-endpoint=${ticket.url}`, `auth-token=${ticket.token}`,
  ], {
    env: { ...process.env, GST_PLUGIN_PATH: whipsinkDir(), GST_DEBUG: 'whipsink:5',
      GST_DEBUG_NO_COLOR: '1' },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let output = '';
  child.stdout.on('data', (d) => { output += d; });
  child.stderr.on('data', (d) => { output += d; });
  const exited = new Promise((resolve) => child.on('exit', (code, signal) => resolve(code ?? signal)));
  return {
    output: () => output,
    exited,
    async stop() {
      if (child.exitCode === null && child.signalCode === null) child.kill('SIGINT');
      return exited;
    },
    kill() {
      if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
    },
  };
}

// One stream: its room (the generation's LiveKit room is "<room>:<generation>") and the
// publisher's ticket. The device is the stream's own, so every ticket for the stream names one
// identity.
async function openStream(sfu) {
  const room = randomUUID();
  const device = randomUUID();
  await sfu.open(room, 1, 0);
  const ticket = await sfu.ticket(room, 1, 'streamer', device, 'publisher');
  return { room, device, ticket, name: `${room}:1`, identity: `streamer/${device}` };
}

test('a whipsink publisher is exactly one producer, and a second session replaces it', async () => {
  const sfu = startSignalling();
  const sources = [];
  const metrics = {};
  try {
    const stream = await openStream(sfu);
    metrics.room = stream.name;
    sources.push(whipsink(stream.ticket));
    await expect.poll(() => producers(stream.name), { timeout: 20_000 })
      .toEqual([{ identity: stream.identity, sid: expect.any(String), kinds: ['AUDIO', 'VIDEO'] }]);
    const [first] = await producers(stream.name);

    // The same stream publishing again, as an encoder that reconnects does: one identity, so
    // the new session takes the old one's place instead of standing beside it.
    sources.push(whipsink(stream.ticket));
    await expect.poll(async () => (await producers(stream.name)).map((p) => p.sid),
      { timeout: 20_000 }).toEqual([expect.not.stringMatching(`^${first.sid}$`)]);
    const now = await producers(stream.name);
    expect(now).toHaveLength(1);
    expect(now[0].kinds).toEqual(['AUDIO', 'VIDEO']);
    metrics.producers = now;
    await sfu.close(stream.room, 1);
  } finally {
    for (const source of sources) source.kill();
    sfu.stop();
    metricsFile('one-producer', metrics);
  }
});

test("whipsink's DELETE ends the session at once", async () => {
  const sfu = startSignalling();
  let source;
  const metrics = {};
  try {
    const stream = await openStream(sfu);
    metrics.room = stream.name;
    source = whipsink(stream.ticket);
    await expect.poll(async () => (await producers(stream.name)).length, { timeout: 20_000 })
      .toBe(1);

    const start = Date.now();
    expect(await source.stop()).toBe(0);
    expect(source.output()).toMatch(/Response to DELETE : 200 OK/);
    await expect.poll(async () => (await participants(stream.name)).length,
      { timeout: kDeleteBoundMs, intervals: [100] }).toBe(0);
    metrics.goneAfterMs = Date.now() - start;
    await sfu.close(stream.room, 1);
  } finally {
    source?.kill();
    sfu.stop();
    metricsFile('delete', metrics);
  }
});

test('a candidate trickled after the answer is honoured', async () => {
  const sfu = startSignalling();
  const pageServer = await servePage();
  // Host candidates as addresses, not the mDNS names Chrome gives them by default, which
  // LiveKit does not resolve: the server must be able to send checks to what it is trickled.
  const { server, browser } = await launchPeer(
    { args: ['--disable-features=WebRtcHideLocalIpsWithMdns'] });
  const metrics = {};
  try {
    const stream = await openStream(sfu);
    metrics.room = stream.name;
    const page = await browser.newPage();
    await page.goto(`http://127.0.0.1:${pageServer.address().port}/`);
    const publish = await page.evaluate((t) => window.whipTrickled(t), stream.ticket);
    metrics.publish = publish;
    expect(publish.created).toBe(201);
    // 204: LiveKit parsed the fragment and gave the candidates to the session's ICE agent.
    expect(publish.patched).toBe(204);
    // Otherwise the browser could have checked towards the server's own candidates.
    expect(publish.droppedFromAnswer).toBeGreaterThan(0);
    expect(publish.trickled.length).toBeGreaterThan(0);

    await expect.poll(() => page.evaluate(() => window.whipPath().then((p) => p.state)),
      { timeout: 20_000 }).toBe('connected');
    const selected = await page.evaluate(() => window.whipPath());
    metrics.selected = selected;
    // The browser learnt the server's address only from the server's own check (a peer
    // reflexive candidate), which reached the socket of a candidate the PATCH carried. Its port
    // names that socket; the address can read as loopback, as a check between two addresses
    // of one host travels over the loopback interface.
    expect(selected.remote).toBe('prflx');
    const port = (address) => address.slice(address.lastIndexOf(':') + 1);
    expect(publish.trickled.map(port)).toContain(port(selected.local));
    await expect.poll(async () => (await producers(stream.name)).map((p) => p.kinds),
      { timeout: 20_000 }).toEqual([['AUDIO', 'VIDEO']]);

    expect(await page.evaluate(() => window.whipEnd())).toBe(200);
    await expect.poll(async () => (await participants(stream.name)).length,
      { timeout: kDeleteBoundMs, intervals: [100] }).toBe(0);
    await sfu.close(stream.room, 1);
  } finally {
    sfu.stop();
    await browser.close().catch(() => {});
    await server.close().catch(() => {});
    pageServer.close();
    metricsFile('trickle', metrics);
  }
});

// The M31 packager as tests/e2e runs it, but writing to a directory: ULW_STORAGE=fs.
function startPackager(streamId, passphrase, root) {
  const work = path.join(root, 'scratch');
  mkdirSync(work, { recursive: true });
  const child = spawn(path.join(build, 'apps/live-packager/live_packager'), [], {
    env: {
      PATH: process.env.PATH,
      ULW_STREAM_ID: streamId,
      ULW_LIVE_INGEST_PORT: '0',
      ULW_LIVE_SRT_PASSPHRASE: passphrase,
      ULW_LIVE_SEGMENT_SECONDS: String(kSegmentSeconds),
      ULW_STORAGE: 'fs',
      ULW_FS_ROOT: path.join(root, 'store'),
      ULW_SCRATCH_DIR: work,
      ULW_SANDBOX_BIN: path.join(build, 'apps/live-packager/ulw_sandbox'),
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let output = '';
  const collect = (d) => { output += d; };
  child.stdout.on('data', collect);
  child.stderr.on('data', collect);
  const exited = new Promise((resolve) => child.on('exit', (code, signal) => resolve(code ?? signal)));
  return {
    output: () => output,
    exited,
    playlist: path.join(root, 'store', 'objects', 'live', streamId, 'index.m3u8'),
    kill() {
      if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
    },
  };
}

function readPlaylist(file) {
  if (!existsSync(file)) return null;
  const text = readFileSync(file, 'utf8');
  return {
    sequence: Number(/#EXT-X-MEDIA-SEQUENCE:(\d+)/.exec(text)?.[1] ?? -1),
    segments: text.split('\n').filter((line) => line.endsWith('.m4s')),
    ended: text.includes('#EXT-X-ENDLIST'),
    target: Number(/#EXT-X-TARGETDURATION:(\d+)/.exec(text)?.[1] ?? -1),
  };
}

// A packager for a new stream, a whipsink publisher in the stream's room, and the relay between
// them: the call a stream service makes once its publisher is in. The recorder joins the room,
// takes that one participant, and calls the packager's listener (ADR-0046).
async function relayedStream(sfu, root, metrics) {
  const streamId = `whip-${randomBytes(6).toString('hex')}`;
  const passphrase = randomBytes(16).toString('hex');
  const packager = startPackager(streamId, passphrase, root);
  metrics.stream = streamId;
  await expect.poll(() => /ingest=127\.0\.0\.1:(\d+)/.exec(packager.output()) !== null,
    { timeout: 30_000 }).toBe(true);
  const port = /ingest=127\.0\.0\.1:(\d+)/.exec(packager.output())[1];

  const stream = await openStream(sfu);
  metrics.room = stream.name;
  const source = whipsink(stream.ticket);
  await expect.poll(async () => (await producers(stream.name)).length, { timeout: 20_000 })
    .toBe(1);
  const relayed = Date.now();
  await sfu.relay(stream.room, 1, 'streamer', stream.device, kSegmentSeconds,
    `srt://127.0.0.1:${port}?streamid=${streamId}&passphrase=${passphrase}`);
  return { packager, stream, source, relayed };
}

async function segmentsListed(packager, atLeast, sequences = []) {
  await expect.poll(() => {
    const playlist = readPlaylist(packager.playlist);
    if (playlist) sequences.push(playlist.sequence);
    return playlist?.segments.length ?? 0;
  }, { timeout: 60_000, intervals: [250] }).toBeGreaterThanOrEqual(atLeast);
}

async function streamEnded(packager, sequences = []) {
  await expect.poll(() => {
    const playlist = readPlaylist(packager.playlist);
    sequences.push(playlist.sequence);
    return playlist.ended;
  }, { timeout: 30_000, intervals: [250] }).toBe(true);
}

test('a WHIP publish becomes a live playlist that ends with the session', async () => {
  const sfu = startSignalling();
  const root = mkdtempSync(path.join(tmpdir(), 'ulw-ingest-'));
  const metrics = {};
  let relay;
  try {
    relay = await relayedStream(sfu, root, metrics);
    const { packager, stream, source } = relay;
    const sequences = [];
    await segmentsListed(packager, 3, sequences);
    metrics.threeSegmentsAfterMs = Date.now() - relay.relayed;
    const live = readPlaylist(packager.playlist);
    expect(live.ended).toBe(false);
    expect(live.target).toBe(kSegmentSeconds);

    const deleted = Date.now();
    expect(await source.stop()).toBe(0);
    expect(source.output()).toMatch(/Response to DELETE : 200 OK/);
    // The publisher gone, the recorder's participant relay ends, its SRT caller closes, and
    // the packager ends the stream (ADR-0047).
    await streamEnded(packager, sequences);
    metrics.endedAfterDeleteMs = Date.now() - deleted;
    expect(await packager.exited).toBe(0);
    expect(packager.output()).toMatch(/publisher disconnected/);

    const ended = readPlaylist(packager.playlist);
    metrics.segments = ended.sequence + ended.segments.length;
    for (const segment of ended.segments) {
      expect(existsSync(path.join(path.dirname(packager.playlist), segment))).toBe(true);
    }
    expect(sequences).toEqual([...sequences].sort((a, b) => a - b));
    await sfu.close(stream.room, 1);
  } finally {
    relay?.source.kill();
    relay?.packager.kill();
    sfu.stop();
    rmSync(root, { recursive: true, force: true });
    metricsFile('packager', metrics);
  }
});

// M25 acceptance: two headless Chrome peers join one call with tickets the SFU adapter issues,
// reach ICE connected, and receive each other's RTP for 10 s; then one peer's network vanishes
// and the other sees it leave, within a measured bound, with its own call intact. And a
// participant put out of a call stays out, whatever credential it kept.
import { chromium, expect, test } from '@playwright/test';
import { spawn } from 'node:child_process';
import { createHmac, randomUUID } from 'node:crypto';
import { mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import path from 'node:path';
import { createInterface } from 'node:readline';

const here = path.dirname(new URL(import.meta.url).pathname);
const harness = process.env.ULW_CALL_HARNESS;

// The acceptance window for media flow.
const kFlowMs = 10_000;
// LiveKit v1.13.7 gives up on a silent peer after 10 s without ICE traffic (disconnected), 5 s
// more (failed) and a 5 s cleanup wait: 20 s (pkg/rtc/transport.go and participant.go). On top:
// up to 2 s for pion to notice, as it checks on its keepalive tick (transport.go), the 100 ms
// poll below, and the leave's hop to the other peer over its open signal connection, which takes
// milliseconds on loopback. That is 22.1 s at worst; 25 s leaves about 3 s for a loaded machine.
// Measured: 20.0 to 21.9 s.
const kDropBoundMs = 25_000;

// The call handler's stand-in: one harness process for the whole test, holding the rooms it
// opened as the handler would, driven one command line at a time.
function startSignalling() {
  const child = spawn(harness, [], { stdio: ['pipe', 'pipe', 'inherit'] });
  const lines = createInterface({ input: child.stdout })[Symbol.asyncIterator]();
  const send = async (...words) => {
    child.stdin.write(`${words.join(' ')}\n`);
    const { value, done } = await lines.next();
    if (done) throw new Error(`harness exited during: ${words.join(' ')}`);
    if (value.startsWith('error')) throw new Error(`${words.join(' ')}: ${value}`);
    return value;
  };
  return {
    open: (room, generation) => send('open', room, generation, 2),
    ticket: async (room, generation, user, device, role = 'member') =>
      JSON.parse(await send('join', room, generation, user, device, role)),
    close: (room, generation) => send('close', room, generation),
    stop: () => child.stdin.end(),
  };
}

function servePage() {
  const files = {
    '/': ['call.html', 'text/html'],
    '/livekit-client.umd.js': ['node_modules/livekit-client/dist/livekit-client.umd.js',
      'text/javascript'],
  };
  const server = createServer((req, res) => {
    const file = files[req.url];
    if (!file) {
      res.writeHead(404).end();
      return;
    }
    res.writeHead(200, { 'content-type': file[1] }).end(readFileSync(path.join(here, file[0])));
  });
  // Outside mode (run.sh) forwards a fixed port from the outside peer's loopback to this host,
  // so the page server listens there on every address; otherwise any loopback port will do.
  const port = Number(process.env.ULW_CALL_PAGE_PORT ?? 0);
  const address = port === 0 ? '127.0.0.1' : '0.0.0.0';
  return new Promise((resolve) => server.listen(port, address, () => resolve(server)));
}

// Every process of one browser: the browser itself and everything it started.
function processTree(root) {
  const parents = new Map();
  for (const entry of readdirSync('/proc')) {
    if (!/^\d+$/.test(entry)) continue;
    try {
      const stat = readFileSync(`/proc/${entry}/stat`, 'utf8');
      // The command name is parenthesised and may hold spaces; the ppid follows its close.
      const ppid = Number(stat.slice(stat.lastIndexOf(')') + 2).split(' ')[1]);
      parents.set(Number(entry), ppid);
    } catch {
      // Exited while being listed.
    }
  }
  const tree = [root];
  for (let i = 0; i < tree.length; ++i) {
    for (const [pid, ppid] of parents) if (ppid === tree[i]) tree.push(pid);
  }
  return tree;
}

// A browser server rather than a plain launch: only the server exposes its process, which the
// drop below has to freeze. An outside peer runs through run.sh's wrapper, in another network
// namespace.
const outsideChrome = process.env.ULW_CALL_OUTSIDE_CHROME;

async function launchPeer({ outside = false } = {}) {
  const server = await chromium.launchServer({
    headless: true,
    executablePath: outside ? outsideChrome : process.env.ULW_E2E_CHROME,
    args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream'],
  });
  const browser = await chromium.connect(server.wsEndpoint());
  return { server, browser };
}

async function received(page) {
  const pcs = await page.evaluate(() => window.mediaStats());
  return pcs.reduce((sum, pc) => ({ audio: sum.audio + pc.inbound.audio,
    video: sum.video + pc.inbound.video }), { audio: 0, video: 0 });
}

// The candidate types of each live connection's selected pair: "relay" on the local side means
// the media goes through TURN.
async function selectedPaths(page) {
  return page.evaluate(() => window.selectedPaths());
}

async function allConnected(page) {
  const pcs = await page.evaluate(() => window.mediaStats());
  return pcs.length > 0 && pcs.every((pc) => pc.ice === 'connected' || pc.ice === 'completed');
}

test('two peers exchange media and a dropped peer is detected', async () => {
  const room = randomUUID();
  const metrics = { room };
  const pageServer = await servePage();
  const pageUrl = `http://127.0.0.1:${pageServer.address().port}/`;
  const browsers = [];
  const sfu = startSignalling();
  try {
    await sfu.open(room, 1);
    const peers = [];
    for (const user of ['alice', 'bob']) {
      const device = randomUUID();
      const ticket = await sfu.ticket(room, 1, user, device);
      const outside = user === 'bob' && outsideChrome !== undefined;
      const { server, browser } = await launchPeer({ outside });
      browsers.push(server);
      const page = await browser.newPage();
      await page.goto(pageUrl);
      peers.push({ user, device, server, page, ticket, outside });
    }
    const started = Date.now();
    for (const peer of peers) {
      peer.identity = await peer.page.evaluate((t) => window.join(t), peer.ticket);
    }
    const [alice, bob] = peers;

    for (const peer of peers) {
      await expect.poll(() => allConnected(peer.page), { timeout: 30_000 }).toBe(true);
      await expect.poll(async () => {
        const r = await received(peer.page);
        return r.audio > 0 && r.video > 0;
      }, { timeout: 30_000 }).toBe(true);
    }
    metrics.connectedMs = Date.now() - started;
    for (const peer of peers) {
      metrics[`${peer.user}Paths`] = await selectedPaths(peer.page);
      if (peer.outside) {
        const local = metrics[`${peer.user}Paths`].map((p) => p.local);
        expect(local.length, `${peer.user} has no selected pair`).toBeGreaterThan(0);
        expect(local, `${peer.user} is not relayed`).toEqual(local.map(() => 'relay'));
      }
    }

    // Sampled once a second for the whole window: packets must keep arriving in both
    // directions, not merely have arrived once. The wait below is the sampling period, not a
    // wait for something to happen; there is nothing to wait on but time.
    const samples = { alice: [await received(alice.page)], bob: [await received(bob.page)] };
    const flowEnd = Date.now() + kFlowMs;
    while (Date.now() < flowEnd) {
      await alice.page.waitForTimeout(1000);
      samples.alice.push(await received(alice.page));
      samples.bob.push(await received(bob.page));
    }
    for (const [who, series] of Object.entries(samples)) {
      for (let i = 1; i < series.length; ++i) {
        expect(series[i].audio, `${who} audio stalled`).toBeGreaterThan(series[i - 1].audio);
        expect(series[i].video, `${who} video stalled`).toBeGreaterThan(series[i - 1].video);
      }
    }
    metrics.packetsIn10s = Object.fromEntries(Object.entries(samples).map(([who, s]) =>
      [who, { audio: s.at(-1).audio - s[0].audio, video: s.at(-1).video - s[0].video }]));
    expect(await allConnected(alice.page)).toBe(true);
    expect(await allConnected(bob.page)).toBe(true);

    // Bob's network vanishes: every process of his browser stops, so nothing he would send
    // leaves and nothing sent to him is answered, which is what the SFU sees of a peer whose
    // link died. Closing his page instead would send a polite leave.
    const frozen = processTree(bob.server.process().pid);
    const droppedAt = Date.now();
    for (const pid of frozen) process.kill(pid, 'SIGSTOP');
    try {
      await expect.poll(() => alice.page.evaluate((who) =>
        window.events.find((e) => e.type === 'participant-disconnected' && e.who === who),
      bob.identity), { timeout: kDropBoundMs, intervals: [100] }).toBeTruthy();
      const events = await alice.page.evaluate(() => window.events);
      const gone = events.find((e) => e.type === 'participant-disconnected');
      metrics.dropDetectedMs = gone.at - droppedAt;
      const unsubscribed = events.filter((e) => e.type === 'track-unsubscribed' && e.who === bob.identity);
      expect(unsubscribed.map((e) => e.kind).sort()).toEqual(['audio', 'video']);
      metrics.aliceEventsAfterDrop = events.filter((e) => e.at >= droppedAt)
        .map((e) => ({ ...e, at: e.at - droppedAt }));
      expect(events.some((e) => e.type === 'disconnected'), 'alice lost the call').toBe(false);
      expect(events.some((e) => e.type === 'reconnecting'), 'alice had to reconnect').toBe(false);
      // Ending cleanly, concretely: LiveKit renegotiates bob's tracks off alice's connection,
      // so their inbound RTP streams leave her statistics altogether (bob was the only one
      // sending to her), with her connections up throughout. The MediaStreamTracks themselves
      // stay "live": the SDK detaches them, but only the page could stop them.
      await expect.poll(() => received(alice.page), { timeout: 5_000 })
        .toEqual({ audio: 0, video: 0 });
      metrics.streamsRemovedMs = Date.now() - droppedAt - metrics.dropDetectedMs;
      expect(await allConnected(alice.page)).toBe(true);
    } finally {
      for (const pid of frozen) {
        try {
          process.kill(pid, 'SIGCONT');
        } catch {
          // Already gone.
        }
      }
    }

    // The room is closed through the adapter; the remaining peer is told why.
    const closedAt = Date.now();
    await sfu.close(room, 1);
    await expect.poll(() => alice.page.evaluate(() =>
      window.events.find((e) => e.type === 'disconnected')), { timeout: 10_000 }).toBeTruthy();
    const ended = await alice.page.evaluate(() => window.events.find((e) => e.type === 'disconnected'));
    metrics.closeToDisconnectMs = ended.at - closedAt;
    metrics.closeReason = ended.reason;
    expect(ended.reason).toBe(await alice.page.evaluate(() =>
      LivekitClient.DisconnectReason.ROOM_DELETED));
  } finally {
    sfu.stop();
    for (const browser of browsers) await browser.close().catch(() => {});
    pageServer.close();
    console.log(JSON.stringify(metrics));
    mkdirSync(path.join(here, 'test-results'), { recursive: true });
    writeFileSync(path.join(here, 'test-results', `metrics-${room}.json`), JSON.stringify(metrics, null, 2));
  }
});

// The review's probe of a removal that did not hold. LiveKit hands every client a fresh token as
// soon as it joins and keeps renewing it, so a client that is put out still holds a credential
// good for 10 minutes; only closing the generation that credential names keeps it out.
test('an expelled participant cannot return, even with its refreshed token', async () => {
  const room = randomUUID();
  const metrics = { room };
  const pageServer = await servePage();
  const pageUrl = `http://127.0.0.1:${pageServer.address().port}/`;
  const browsers = [];
  const sfu = startSignalling();
  try {
    await sfu.open(room, 1);
    const peers = {};
    for (const user of ['alice', 'mallory']) {
      const device = randomUUID();
      const ticket = await sfu.ticket(room, 1, user, device);
      const { server, browser } = await launchPeer();
      browsers.push(server);
      const page = await browser.newPage();
      await page.goto(pageUrl);
      await page.evaluate((t) => window.join(t), ticket);
      peers[user] = { device, page, ticket };
    }
    const { alice, mallory } = peers;

    await expect.poll(() => mallory.page.evaluate(() => window.room.engine.token),
      { timeout: 10_000 }).not.toBe(mallory.ticket.token);
    const refreshed = await mallory.page.evaluate(() => window.room.engine.token);

    // Expulsion, as the call handler performs it: the next generation opens, the members who
    // stay are ticketed into it, and the old one closes under everyone still in it.
    const expelledAt = Date.now();
    await sfu.open(room, 2);
    const moved = await sfu.ticket(room, 2, 'alice', alice.device);
    await alice.page.evaluate((t) => window.join(t), moved);
    await sfu.close(room, 1);
    await expect.poll(() => mallory.page.evaluate(() =>
      window.events.find((e) => e.type === 'disconnected')), { timeout: 10_000 }).toBeTruthy();
    metrics.expelledMs = Date.now() - expelledAt;

    const withRefreshed = await mallory.page.evaluate((t) => window.tryConnect(t),
      { url: mallory.ticket.url, token: refreshed });
    const withTicket = await mallory.page.evaluate((t) => window.tryConnect(t), mallory.ticket);
    metrics.retryWithRefreshed = withRefreshed;
    metrics.retryWithTicket = withTicket;
    expect(withRefreshed.admitted, 'the refreshed token let mallory back in').toBe(false);
    expect(withTicket.admitted, 'the ticket let mallory back in').toBe(false);

    // Alice carried on in the new generation.
    expect(await alice.page.evaluate(() => window.room.state)).toBe('connected');
    await sfu.close(room, 2);
  } finally {
    sfu.stop();
    for (const browser of browsers) await browser.close().catch(() => {});
    pageServer.close();
    console.log(JSON.stringify(metrics));
  }
});

// Whether LiveKit still has a room, asked directly: the test's own view, not the adapter's.
async function liveKitHasRoom(name) {
  const b64 = (x) => Buffer.from(JSON.stringify(x)).toString('base64url');
  const now = Math.floor(Date.now() / 1000);
  const unsigned = `${b64({ alg: 'HS256', typ: 'JWT' })}.${b64({
    iss: process.env.LIVEKIT_API_KEY, nbf: now, exp: now + 10, video: { roomList: true } })}`;
  const signature = createHmac('sha256', process.env.LIVEKIT_API_SECRET).update(unsigned)
    .digest('base64url');
  const response = await fetch(`${process.env.LIVEKIT_API_URL}/twirp/livekit.RoomService/ListRooms`, {
    method: 'POST',
    headers: { 'content-type': 'application/json', authorization: `Bearer ${unsigned}.${signature}` },
    body: JSON.stringify({ names: [name] }),
  });
  expect(response.status).toBe(200);
  return ((await response.json()).rooms ?? []).length > 0;
}

// A handle held past LiveKit's idle timeout, as the call handler holds one while a callee's
// phone rings: the room behind it is gone, and the next join must bring it back rather than
// hand out a ticket to nowhere. The harness opens the room once and never again.
test('a join through a handle whose room went idle opens the room again', async () => {
  const room = randomUUID();
  const metrics = { room };
  const pageServer = await servePage();
  const browsers = [];
  const sfu = startSignalling();
  try {
    await sfu.open(room, 1);
    const openedAt = Date.now();
    // The adapter's 60 s empty timeout, and LiveKit's own sweep on top.
    await expect.poll(() => liveKitHasRoom(`${room}:1`),
      { timeout: 90_000, intervals: [1000] }).toBe(false);
    metrics.droppedAfterMs = Date.now() - openedAt;

    const ticket = await sfu.ticket(room, 1, 'alice', randomUUID());
    expect(await liveKitHasRoom(`${room}:1`)).toBe(true);
    const { server, browser } = await launchPeer();
    browsers.push(server);
    const page = await browser.newPage();
    await page.goto(`http://127.0.0.1:${pageServer.address().port}/`);
    await page.evaluate((t) => window.join(t), ticket);
    expect(await page.evaluate(() => window.room.state)).toBe('connected');
    await sfu.close(room, 1);
  } finally {
    sfu.stop();
    for (const browser of browsers) await browser.close().catch(() => {});
    pageServer.close();
    console.log(JSON.stringify(metrics));
  }
});

// M30's way in: a publish-only ticket, used for WHIP with no SDK at all, reaches a call member.
test('a publisher ticket ingests over WHIP and members receive it', async () => {
  const room = randomUUID();
  const metrics = { room };
  const pageServer = await servePage();
  const pageUrl = `http://127.0.0.1:${pageServer.address().port}/`;
  const browsers = [];
  const sfu = startSignalling();
  try {
    await sfu.open(room, 1);
    const pages = [];
    for (let i = 0; i < 2; ++i) {
      const { server, browser } = await launchPeer();
      browsers.push(server);
      const page = await browser.newPage();
      await page.goto(pageUrl);
      pages.push(page);
    }
    const [viewer, source] = pages;
    await viewer.evaluate((t) => window.join(t),
      await sfu.ticket(room, 1, 'alice', randomUUID()));
    const publisher = await sfu.ticket(room, 1, 'streamer', randomUUID(), 'publisher');
    expect(publisher.url).toBe(`${process.env.LIVEKIT_API_URL}/whip/v1`);
    metrics.whipStatus = await source.evaluate((t) => window.whipPublish(t), publisher);
    expect(metrics.whipStatus).toBe(201);

    await expect.poll(() => viewer.evaluate(() => window.events
      .filter((e) => e.type === 'track-subscribed' && e.who.startsWith('streamer/'))
      .map((e) => e.kind).sort()), { timeout: 20_000 }).toEqual(['audio', 'video']);
    const before = await received(viewer);
    await expect.poll(async () => {
      const now = await received(viewer);
      return now.audio > before.audio && now.video > before.video;
    }, { timeout: 10_000 }).toBe(true);
    await sfu.close(room, 1);
  } finally {
    sfu.stop();
    for (const browser of browsers) await browser.close().catch(() => {});
    pageServer.close();
    console.log(JSON.stringify(metrics));
  }
});

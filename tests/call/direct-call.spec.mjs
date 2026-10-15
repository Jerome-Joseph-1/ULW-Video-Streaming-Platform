// 1:1 calls through the product, in Chrome. Alice and Bob each open the chat WebSocket on a
// different node of a two-node cluster; Alice opens their direct chat over chat (open_direct,
// ADR-0096) and Bob hears he was listed. Both join the room, ask for the call, and take the
// ticket chat answers to LiveKit with the pinned livekit-client SDK, publishing the fake camera
// and microphone. Each must decode the other's audio and video. A second device of Alice's is
// refused by LiveKit (the call holds two), Carol is refused by chat (not a member); when Bob
// leaves Alice sees him go, and Bob, back on a new socket, gets a fresh ticket and the call
// resumes. Every ticket comes from chat_server; LiveKit's key is only in chat's environment
// (docs/integration/calls.md, ADR-0087).
import { chromium, expect, test } from '@playwright/test';
import { spawn } from 'node:child_process';
import { createHmac, randomUUID } from 'node:crypto';

import { freePort, startChat } from './chat-stack.mjs';
import { servePage } from './peers.mjs';

// Frames and seconds of play that show media is decoding, not merely arriving: the fake camera
// sends 30 frames a second, so these take about a second of flow beyond the first sample.
const kFrames = 20;
const kPlayedSeconds = 0.5;

async function decoded(page, who) {
  return page.evaluate((w) => window.decoded(w), who);
}

// Waits until `page` plays `who`'s video (a decoded frame of known size on the element) and
// audio, then until both have moved on from that first sample: more frames decoded, more audio
// samples, more packets, and both elements' play positions advanced.
async function expectDecoding(page, who) {
  let first;
  await expect.poll(async () => {
    first = await decoded(page, who);
    return Boolean(first.video && first.audio && first.video.videoWidth > 0 &&
      first.video.readyState >= 2 && first.video.framesDecoded > 0 &&
      first.audio.totalSamplesReceived > 0);
  }, { message: `no media of ${who} decoded`, timeout: 30_000 }).toBe(true);
  await expect.poll(async () => {
    const now = await decoded(page, who);
    return now.video !== null && now.audio !== null &&
      now.video.framesDecoded >= first.video.framesDecoded + kFrames &&
      now.video.currentTime >= first.video.currentTime + kPlayedSeconds &&
      now.video.packetsReceived > first.video.packetsReceived &&
      now.audio.totalSamplesReceived > first.audio.totalSamplesReceived &&
      now.audio.packetsReceived > first.audio.packetsReceived &&
      now.audio.currentTime >= first.audio.currentTime + kPlayedSeconds;
  }, { message: `${who}'s media stopped decoding`, timeout: 15_000 }).toBe(true);
}

// What LiveKit itself holds in a room, read with a server token of the test's own (it only looks:
// every ticket the clients use comes from chat).
async function participants(roomName) {
  const encode = (value) => Buffer.from(JSON.stringify(value)).toString('base64url');
  const now = Math.floor(Date.now() / 1000);
  const unsigned = `${encode({ alg: 'HS256', typ: 'JWT' })}.${encode({
    iss: process.env.LIVEKIT_API_KEY, nbf: now, exp: now + 60,
    video: { roomAdmin: true, room: roomName } })}`;
  const signature = createHmac('sha256', process.env.LIVEKIT_API_SECRET).update(unsigned)
    .digest('base64url');
  const response = await fetch(
    `${process.env.LIVEKIT_API_URL}/twirp/livekit.RoomService/ListParticipants`, {
      method: 'POST',
      headers: { 'content-type': 'application/json',
        authorization: `Bearer ${unsigned}.${signature}` },
      body: JSON.stringify({ room: roomName }),
    });
  expect(response.status).toBe(200);
  return (await response.json()).participants ?? [];
}

async function events(page, type, who) {
  return page.evaluate(([t, w]) => window.events.filter((e) => e.type === t && e.who === w),
    [type, who]);
}

test('a direct chat call, asked for on chat and connected in the browser', async ({}, info) => {
  const pageServer = await servePage('direct-call.html');
  const origin = `http://127.0.0.1:${pageServer.address().port}`;
  let chat;
  let browser;
  try {
    chat = await startChat({ origin });
    const [nodeA, nodeB] = chat.nodes;
    browser = await chromium.launch({
      executablePath: process.env.ULW_E2E_CHROME,
      args: ['--use-fake-ui-for-media-stream', '--use-fake-device-for-media-stream',
        // The remote audio element plays without a click, as it would after the user's answer.
        '--autoplay-policy=no-user-gesture-required'],
    });

    // A browser profile of `user`'s, signed in with the cookie chat reads, on the chat page,
    // with its chat socket open to `node`.
    const client = async (user, node) => {
      const context = await browser.newContext();
      await context.addCookies([{ name: 'auth_token', value: chat.mint(user), domain: '127.0.0.1',
        path: '/', httpOnly: true, sameSite: 'Lax' }]);
      const page = await context.newPage();
      page.on('pageerror', (e) => console.log(`${user}: ${e}`));
      await page.goto(`${origin}/`);
      await page.evaluate((url) => window.openChat(url), node.ws);
      return { user, context, page, device: randomUUID() };
    };
    const ask = (c, cmd) => c.page.evaluate((x) => window.command(x), cmd);
    // The direct chat's room, once Alice has opened it.
    let room;
    const join = (c) => ask(c, { type: 'join', room, kind: 'direct' });
    const call = (c, device = c.device) => ask(c, { type: 'call', room, device });
    const tickets = async () =>
      Promise.all(chat.nodes.map((n) => chat.metric(n, 'call_tickets_total')));

    const alice = await client('alice', nodeA);
    const bob = await client('bob', nodeB);

    await test.step('alice opens the direct chat with bob, and both are its members', async () => {
      // Opened over chat, as the product does: the pair's room, the same whichever of them asks.
      const opened = await ask(alice, { type: 'open_direct', user: 'bob' });
      expect(opened).toMatchObject({ type: 'direct', user: 'bob' });
      room = opened.room;
      expect(room).toMatch(/^03[0-9a-f]{6}-[0-9a-f]{4}-8[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
      // Bob, on the other node, hears that he was listed, without asking.
      await expect.poll(() => bob.page.evaluate((r) => window.chatFrames.filter((f) =>
        f.type === 'member' && f.room === r && f.user === 'bob' && f.change === 'added').length,
      room), { timeout: 10_000 }).toBe(1);
      expect(await ask(bob, { type: 'open_direct', user: 'alice' }))
        .toMatchObject({ type: 'direct', room, user: 'alice' });
      expect(await join(alice)).toMatchObject({ type: 'joined', room });
      expect(await join(bob)).toMatchObject({ type: 'joined', room });
    });

    let aliceTicket;
    let bobTicket;
    await test.step('each member asks chat for the call and gets a ticket', async () => {
      const before = await tickets();
      const issuedAt = Math.floor(Date.now() / 1000);
      aliceTicket = await call(alice);
      bobTicket = await call(bob);
      for (const t of [aliceTicket, bobTicket]) {
        expect(t).toMatchObject({ type: 'ticket', room, url: process.env.LIVEKIT_CLIENT_URL });
        expect(t.expires_at).toBeGreaterThanOrEqual(issuedAt + 59);
        expect(t.expires_at).toBeLessThanOrEqual(issuedAt + 62);
      }
      expect(bobTicket.token).not.toBe(aliceTicket.token);
      // The room's owner issued both, whichever node each asked on: one of the two tickets
      // crossed between the nodes.
      const issued = (await tickets()).map((n, i) => n - before[i]);
      expect(issued.toSorted()).toEqual([0, 2]);
    });

    await test.step('both connect with their tickets and decode each other', async () => {
      alice.identity = await alice.page.evaluate((t) => window.connectCall(t), aliceTicket);
      bob.identity = await bob.page.evaluate((t) => window.connectCall(t), bobTicket);
      expect(alice.identity).toBe(`alice/${alice.device}`);
      expect(bob.identity).toBe(`bob/${bob.device}`);
      await expectDecoding(alice.page, bob.identity);
      await expectDecoding(bob.page, alice.identity);
    });

    await test.step("alice's second device and carol are refused", async () => {
      // Chat gives Alice's second device a ticket of its own; LiveKit keeps the call to two.
      const second = await client('alice', nodeB);
      expect(await join(second)).toMatchObject({ type: 'joined', room });
      const secondTicket = await call(second);
      expect(secondTicket).toMatchObject({ type: 'ticket', room });
      const refused = await second.page.evaluate((t) => window.tryCall(t), secondTicket);
      expect(refused.admitted, 'LiveKit admitted a third participant').toBe(false);
      // The SDK reports only that its signal connection was refused; what LiveKit holds shows
      // why: the call's two, and not the second device.
      const identities = (await participants(`${room}:1`)).map((p) => p.identity).sort();
      expect(identities).toEqual([alice.identity, bob.identity].sort());
      const secondIdentity = `alice/${second.device}`;
      for (const c of [alice, bob]) {
        expect(await events(c.page, 'participant-connected', secondIdentity)).toHaveLength(0);
      }
      await second.context.close();

      // Carol is not on the member list: chat refuses her join, and so she cannot ask.
      const carol = await client('carol', nodeA);
      expect(await join(carol)).toMatchObject({ type: 'error', reason: 'not_member', room });
      expect(await call(carol)).toMatchObject({ type: 'error', reason: 'not_joined', room });
      await carol.context.close();

      // The call itself went on.
      await expectDecoding(alice.page, bob.identity);
      await expectDecoding(bob.page, alice.identity);
    });

    await test.step('bob leaves and alice sees him go', async () => {
      await bob.page.evaluate(() => window.leaveCall());
      await expect.poll(() => events(alice.page, 'participant-disconnected', bob.identity),
        { timeout: 10_000 }).toHaveLength(1);
      await expect.poll(async () => (await events(alice.page, 'track-unsubscribed', bob.identity))
        .map((e) => e.kind).sort(), { timeout: 10_000 }).toEqual(['audio', 'video']);
      expect(await alice.page.evaluate(() => window.room.state)).toBe('connected');
    });

    await test.step('bob comes back on a new socket, asks again and gets a fresh ticket',
      async () => {
        // As after an app restart: a new chat socket, here on the other node, and a new ask.
        await bob.page.evaluate(() => window.closeChat());
        await bob.page.evaluate((url) => window.openChat(url), nodeA.ws);
        expect(await join(bob)).toMatchObject({ type: 'joined', room });
        const fresh = await call(bob);
        expect(fresh).toMatchObject({ type: 'ticket', room, url: bobTicket.url });
        expect(fresh.token).not.toBe(bobTicket.token);
        expect(fresh.expires_at).toBeGreaterThanOrEqual(bobTicket.expires_at);
        expect(await bob.page.evaluate((t) => window.connectCall(t), fresh)).toBe(bob.identity);
        await expectDecoding(alice.page, bob.identity);
        await expectDecoding(bob.page, alice.identity);
        // Once when he first came, once now.
        expect(await events(alice.page, 'participant-connected', bob.identity)).toHaveLength(2);
      });
  } catch (e) {
    if (chat) {
      await info.attach('chat-nodes.log', { body: chat.output(), contentType: 'text/plain' });
    }
    throw e;
  } finally {
    await browser?.close();
    pageServer.close();
    await chat?.stop();
  }
});

// With an external TURN service (ULW_TURN_MODE=external, ADR-0109) every ticket names the relays
// its device reaches LiveKit through, the answering member's too, whose ticket the room's owner
// on the other node issues: the ticket crosses between the nodes as the owner's answer, which
// must keep them. A coturn on loopback checks the TURN REST API's credentials (hmac) against the
// shared secret, as an operator's would; both browsers connect with nothing but the ticket's
// relays (iceTransportPolicy "relay", as docs/integration/calls.md has a client do), so media
// that decodes went through it.
async function startTurn(secret) {
  const port = await freePort();
  const relayLow = await freePort();
  const child = spawn('turnserver', ['-n', '--no-cli', '--no-tls', '--no-dtls', '--fingerprint',
    '--listening-ip=127.0.0.1', '--relay-ip=127.0.0.1', `--listening-port=${port}`,
    '--use-auth-secret', `--static-auth-secret=${secret}`, '--realm=ulw.test',
    // LiveKit is on loopback here; coturn refuses loopback peers unless told.
    '--allow-loopback-peers', `--min-port=${relayLow}`, `--max-port=${relayLow + 50}`,
    '--log-file=stdout', '--simple-log'], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  for (const stream of [child.stdout, child.stderr]) stream.on('data', (c) => { output += c; });
  const exited = new Promise((resolve) => child.on('exit', resolve));
  await expect.poll(() => child.exitCode === null && output.includes('Total General servers'),
    { message: 'coturn did not start', timeout: 10_000 }).toBe(true);
  return {
    url: `turn:127.0.0.1:${port}?transport=udp`,
    output: () => output,
    async stop() { child.kill('SIGTERM'); await exited; },
  };
}

test('in external TURN mode a rung and answered call is relayed through the ticket\'s TURN server',
  async ({}, info) => {
    const pageServer = await servePage('direct-call.html');
    const origin = `http://127.0.0.1:${pageServer.address().port}`;
    const secret = randomUUID() + randomUUID();
    let turn;
    let chat;
    let browser;
    try {
      turn = await startTurn(secret);
      const urls = [turn.url];
      chat = await startChat({ origin, env: {
        ULW_TURN_MODE: 'external', ULW_TURN_PROVIDER: 'hmac', ULW_TURN_URLS: urls.join(','),
        ULW_TURN_SHARED_SECRET: secret } });
      for (const node of chat.nodes) expect(node.output).toContain('"turn":"external hmac"');
      const [nodeA, nodeB] = chat.nodes;
      browser = await chromium.launch({
        executablePath: process.env.ULW_E2E_CHROME,
        args: ['--use-fake-ui-for-media-stream', '--use-fake-device-for-media-stream',
          '--autoplay-policy=no-user-gesture-required'],
      });
      const client = async (user, node) => {
        const context = await browser.newContext();
        await context.addCookies([{ name: 'auth_token', value: chat.mint(user),
          domain: '127.0.0.1', path: '/', httpOnly: true, sameSite: 'Lax' }]);
        const page = await context.newPage();
        page.on('pageerror', (e) => console.log(`${user}: ${e}`));
        await page.goto(`${origin}/`);
        await page.evaluate((url) => window.openChat(url), node.ws);
        return { user, page, device: randomUUID() };
      };
      const ask = (c, cmd) => c.page.evaluate((x) => window.command(x), cmd);
      // Not alice and bob: a direct chat's room is its pair's, and LiveKit may still hold the
      // first test's room of that name, full.
      const alice = await client('erin', nodeA);
      const bob = await client('frank', nodeB);
      const { room } = await ask(alice, { type: 'open_direct', user: 'frank' });
      expect(room).toBeTruthy();
      expect(await ask(bob, { type: 'open_direct', user: 'erin' }))
        .toMatchObject({ type: 'direct', room });
      for (const c of [alice, bob]) {
        expect(await ask(c, { type: 'join', room, kind: 'direct' }))
          .toMatchObject({ type: 'joined', room });
      }

      // Each ticket's one relay: the configured URIs, and a credential for that device alone
      // that the secret derives (base64 of HMAC-SHA1 over "<expiry>:<device>").
      const expectRelays = (ticket, device) => {
        expect(ticket.ice_servers, JSON.stringify(ticket)).toHaveLength(1);
        const [relay] = ticket.ice_servers;
        expect(relay.urls).toEqual(urls);
        const [expiry, name] = relay.username.split(':');
        expect(name).toBe(device);
        expect(Number(expiry)).toBeGreaterThan(Math.floor(Date.now() / 1000));
        expect(relay.credential).toBe(
          createHmac('sha1', secret).update(relay.username).digest('base64'));
      };

      const tickets = () =>
        Promise.all(chat.nodes.map((n) => chat.metric(n, 'call_tickets_total')));
      const before = await tickets();
      let ring;
      let answered;
      await test.step('alice rings, bob answers, and both tickets carry the relay', async () => {
        ring = await ask(alice, { type: 'call', room, device: alice.device });
        expect(ring).toMatchObject({ type: 'ticket', room, url: process.env.LIVEKIT_CLIENT_URL });
        expectRelays(ring, alice.device);
        await expect.poll(() => bob.page.evaluate(([r, id]) => window.chatFrames.filter((f) =>
          f.type === 'call_ringing' && f.room === r && f.call === id).length, [room, ring.call]),
        { message: 'bob never rang', timeout: 10_000 }).not.toBe(0);
        answered = await ask(bob, { type: 'call', room, device: bob.device, answer: ring.call });
        expect(answered).toMatchObject({ type: 'ticket', room, call: ring.call });
        expectRelays(answered, bob.device);
        // The room's owner issued both: one of them crossed between the nodes.
        const issued = (await tickets()).map((n, i) => n - before[i]);
        expect(issued.toSorted()).toEqual([0, 2]);
      });

      await test.step('both connect through the relay alone and decode each other', async () => {
        alice.identity = await alice.page.evaluate((t) => window.connectCall(t), ring);
        bob.identity = await bob.page.evaluate((t) => window.connectCall(t), answered);
        await expectDecoding(alice.page, bob.identity);
        await expectDecoding(bob.page, alice.identity);
        for (const c of [alice, bob]) {
          const local = (await c.page.evaluate(() => window.selectedPaths())).map((p) => p.local);
          expect(local.length, `${c.user} has no selected pair`).toBeGreaterThan(0);
          expect(local, `${c.user} is not relayed`).toEqual(local.map(() => 'relay'));
        }
      });
    } catch (e) {
      if (chat) {
        await info.attach('chat-nodes.log', { body: chat.output(), contentType: 'text/plain' });
      }
      if (turn) await info.attach('coturn.log', { body: turn.output(), contentType: 'text/plain' });
      throw e;
    } finally {
      await browser?.close();
      pageServer.close();
      await chat?.stop();
      await turn?.stop();
    }
  });

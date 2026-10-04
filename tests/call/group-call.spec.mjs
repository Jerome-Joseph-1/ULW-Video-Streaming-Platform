// Group calls through the product, in Chrome (M27, ADR-0095): Alice, Bob, Carol and Dave, members
// of a group chat, open the chat WebSocket on two nodes of a two-node cluster, join the room, and
// take the tickets chat answers to LiveKit with the pinned livekit-client SDK, publishing the fake
// camera and microphone. Each of the four must decode the other three: three inbound audio and
// three inbound video streams each. The room's owner is then killed: the call's media goes on
// untouched, and the other node tickets the same generation once it owns the room. Alice, who
// called, puts Dave out: the call moves to the next generation, LiveKit closes the old one under
// everyone, and the three who stay come back with fresh tickets and decode each other again,
// while Dave is refused by chat and by LiveKit. Then they leave, and the last one out ends it.
// Every ticket comes from chat_server; LiveKit's key is only in chat's environment.
import { chromium, expect, test } from '@playwright/test';
import { randomUUID } from 'node:crypto';

import { psql, startChat } from './chat-stack.mjs';
import { servePage } from './peers.mjs';

// As direct-call.spec.mjs: frames and seconds of play that show media is decoding.
const kFrames = 20;
const kPlayedSeconds = 0.5;
// The owner's heartbeat goes stale after 5 s (ADR-0015); the other node then claims the room at
// its next beat and answers. Ten seconds more is room for a loaded machine.
const kTakeoverBoundMs = 15_000;

async function decoded(page, who) {
  return page.evaluate((w) => window.decoded(w), who);
}

// Waits until `page` plays `who`'s video and audio, then until both have moved on from that
// first sample (direct-call.spec.mjs explains each condition).
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

// Every one of `clients` decodes every other one, and receives exactly their streams.
async function expectEveryoneDecodesEveryoneElse(clients) {
  for (const c of clients) {
    for (const other of clients) {
      if (other !== c) await expectDecoding(c.page, other.identity);
    }
    const n = clients.length - 1;
    await expect.poll(() => c.page.evaluate(() => window.inboundSsrcs()),
      { message: `${c.user} does not receive exactly the others' streams`, timeout: 15_000 })
      .toEqual({ audio: n, video: n });
  }
}

// The LiveKit room a ticket admits to, from its claims (read, not verified: LiveKit verifies).
function roomOf(ticket) {
  const claims = JSON.parse(Buffer.from(ticket.token.split('.')[1], 'base64url').toString());
  return claims.video.room;
}

async function frames(page, type, call) {
  return page.evaluate(([t, c]) => window.chatFrames.filter((f) => f.type === t &&
    (c === undefined || f.call === c)), [type, call]);
}

async function events(page, type, who) {
  return page.evaluate(([t, w]) => window.events.filter((e) => e.type === t &&
    (w === undefined || e.who === w)), [type, who]);
}

test('a group call of four on two chat nodes outlives its owner and puts one out',
  async ({}, info) => {
    const pageServer = await servePage('direct-call.html');
    const origin = `http://127.0.0.1:${pageServer.address().port}`;
    const metrics = {};
    let chat;
    let browser;
    try {
      chat = await startChat({ origin });
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
        return { user, context, page, device: randomUUID() };
      };
      const ask = (c, cmd) => c.page.evaluate((x) => window.command(x), cmd);
      const room = randomUUID();
      const join = (c) => ask(c, { type: 'join', room, kind: 'group' });
      const call = (c) => ask(c, { type: 'call', room, device: c.device });
      const connect = async (c, ticket) => {
        c.identity = await c.page.evaluate((t) => window.connectCall(t), ticket);
        expect(c.identity).toBe(`${c.user}/${c.device}`);
      };

      // Alice joins first: her node, A, owns the room. Two members on each node.
      const alice = await client('alice', nodeA);
      const bob = await client('bob', nodeB);
      const carol = await client('carol', nodeA);
      const dave = await client('dave', nodeB);
      const everyone = [alice, bob, carol, dave];

      await test.step('the group chat is created and its members listed', async () => {
        psql(chat.databaseUrl, `BEGIN;
          INSERT INTO chat_rooms (room_id, kind) VALUES ('${room}', 'group_chat');
          INSERT INTO chat_members (room_id, user_id) VALUES ('${room}', 'alice'),
            ('${room}', 'bob'), ('${room}', 'carol'), ('${room}', 'dave');
          COMMIT;`);
        for (const c of everyone) expect(await join(c)).toMatchObject({ type: 'joined', room });
      });

      let first;
      await test.step('alice calls: the three others hear it ring and answer', async () => {
        const ticket = await call(alice);
        expect(ticket).toMatchObject({ type: 'ticket', room, url: process.env.LIVEKIT_CLIENT_URL });
        first = ticket.call;
        expect(roomOf(ticket)).toBe(`${room}:1`);
        await connect(alice, ticket);
        for (const c of [bob, carol, dave]) {
          await expect.poll(() => frames(c.page, 'call_ringing', first),
            { message: `${c.user} never rang`, timeout: 10_000 }).not.toHaveLength(0);
          const answer = await call(c);
          expect(answer).toMatchObject({ type: 'ticket', room, call: first });
          await connect(c, answer);
        }
        await expect.poll(() => frames(alice.page, 'call_answered', first), { timeout: 10_000 })
          .toHaveLength(3);
      });

      await test.step('each of the four decodes the other three', async () => {
        await expectEveryoneDecodesEveryoneElse(everyone);
      });

      await test.step('the owner fails: media goes on, and the other node tickets the same ' +
        'generation', async () => {
        const killedAt = Date.now();
        await nodeA.kill();
        // Alice and Carol lost their chat sockets with node A, and open them on node B.
        for (const c of [alice, carol]) {
          await c.page.evaluate(() => window.closeChat());
          await c.page.evaluate((url) => window.openChat(url), nodeB.ws);
          expect(await join(c)).toMatchObject({ type: 'joined', room });
        }
        // Asked until node B owns the room; each refusal says to retry.
        let again;
        await expect.poll(async () => {
          again = await call(carol);
          return again.type === 'ticket' ? 'ticket' : again.reason;
        }, { timeout: kTakeoverBoundMs, intervals: [500] }).toBe('ticket');
        metrics.ticketAgainAfterOwnerKillMs = Date.now() - killedAt;
        // Node B knew no call: Carol's ticket started one, which rings the others; Alice, Bob
        // and Dave, already in the call, answer it with tickets of their own as soon as it rings
        // (calls.md), and keep their connections.
        first = again.call;
        for (const c of [alice, bob, dave]) {
          await expect.poll(() => frames(c.page, 'call_ringing', first), { timeout: 10_000 })
            .not.toHaveLength(0);
          expect(await call(c)).toMatchObject({ type: 'ticket', call: first });
        }
        // Media never passed through chat: every one of the four still decodes every other,
        // and nobody's call was even interrupted.
        await expectEveryoneDecodesEveryoneElse(everyone);
        for (const c of everyone) {
          expect(await events(c.page, 'disconnected'), `${c.user} lost the call`).toHaveLength(0);
          expect(await events(c.page, 'reconnecting'), `${c.user} reconnected`).toHaveLength(0);
        }
        // The generation the store holds, not a fresh one: Carol's fresh ticket rejoins the same
        // room, in place of her connection.
        expect(roomOf(again)).toBe(`${room}:1`);
        await connect(carol, again);
        await expectEveryoneDecodesEveryoneElse(everyone);
      });

      await test.step('carol, who called now, puts dave out; the three move on', async () => {
        const expelledAt = Date.now();
        const moved = await ask(carol, { type: 'call_expel', room, call: first, user: 'dave' });
        expect(moved).toMatchObject({ type: 'call_moved', call: first, expelled: 'dave',
          by: 'carol' });
        // LiveKit closes generation 1 under all four.
        await expect.poll(() => events(dave.page, 'disconnected'), { timeout: 10_000 })
          .toHaveLength(1);
        expect((await events(dave.page, 'disconnected'))[0].reason).toBe(
          await dave.page.evaluate(() => LivekitClient.DisconnectReason.ROOM_DELETED));
        for (const c of [alice, bob]) {
          await expect.poll(() => frames(c.page, 'call_moved', first), { timeout: 10_000 })
            .toHaveLength(1);
        }
        // Each who stays asks again, and connects to generation 2: the interruption the move
        // costs them.
        const stayed = [alice, bob, carol];
        for (const c of stayed) {
          const ticket = await call(c);
          expect(ticket).toMatchObject({ type: 'ticket', call: first });
          expect(roomOf(ticket)).toBe(`${room}:2`);
          await connect(c, ticket);
        }
        metrics.expelToAllReconnectedMs = Date.now() - expelledAt;
        await expectEveryoneDecodesEveryoneElse(stayed);
        // Dave is refused by chat, and LiveKit admits nobody to the closed generation.
        expect(await call(dave)).toMatchObject({ type: 'error', reason: 'expelled', room });
        for (const c of stayed) {
          expect(await events(c.page, 'participant-connected', dave.identity)).toHaveLength(1);
        }
      });

      await test.step('the members leave, and the last one out ends the call', async () => {
        for (const c of [alice, bob]) {
          expect(await ask(c, { type: 'call_leave', room, call: first }))
            .toMatchObject({ type: 'call_left', by: c.user });
          await c.page.evaluate(() => window.leaveCall());
        }
        await expect.poll(() => frames(carol.page, 'call_left', first), { timeout: 10_000 })
          .toHaveLength(2);
        expect(await ask(carol, { type: 'call_leave', room, call: first }))
          .toMatchObject({ type: 'call_left', by: 'carol' });
        // Everyone in the call; Dave, put out, is not told of it any more.
        for (const c of [alice, bob, carol]) {
          await expect.poll(() => frames(c.page, 'call_ended', first),
            { message: `${c.user} never heard the end`, timeout: 10_000 }).toHaveLength(1);
        }
        expect(await frames(dave.page, 'call_ended', first)).toHaveLength(0);
      });
    } catch (e) {
      if (chat) {
        await info.attach('chat-nodes.log', { body: chat.output(), contentType: 'text/plain' });
      }
      throw e;
    } finally {
      console.log(JSON.stringify(metrics));
      await browser?.close();
      pageServer.close();
      await chat?.stop();
    }
  });

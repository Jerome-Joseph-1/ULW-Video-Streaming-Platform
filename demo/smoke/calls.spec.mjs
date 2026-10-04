// Calls under the conditions that broke a real 1:1 call on the deployment (2026-10-04):
// a ring cancelled and rung again, a ring whose cancel a device never heard (its chat socket was
// reconnecting, as it does each time its token expires), and a microphone the operating system
// takes away mid-call (a phone call on the device) and gives back ended or still muted.
//
// Each test uses a pair of its own, so that the ring limits of one direct chat (5 rings a minute)
// are not spent by another, and each waits for conditions, never for time.
import { test, expect } from '@playwright/test';

test.describe.configure({ mode: 'serial' });

let browser;
const pages = {};

async function open(user) {
  if (pages[user]) return pages[user];
  const context = await browser.newContext({ permissions: ['camera', 'microphone'] });
  const page = await context.newPage();
  page.on('console', (m) => { if (m.type() === 'error') console.log(`[${user}] ${m.text()}`); });
  page.on('pageerror', (e) => console.log(`[${user}] page error: ${e.message}`));
  if (process.env.SMOKE_LOGIN === 'oidc') {
    await page.goto('/');
    await page.locator('#sign-in').click();
    await page.locator('#username').fill(user);
    await page.locator('#password').fill(process.env.SMOKE_PASSWORD ?? 'testtest123');
    await page.locator('#kc-login').click();
  } else {
    await page.goto(`/?user=${user}`);
  }
  await page.waitForFunction(() => window.demo?.ready === true);
  await page.waitForFunction(() => window.demo.chat.open === true);
  pages[user] = page;
  return page;
}

const tab = (page, name) => page.locator(`#tabs button[data-tab="${name}"]`).click();
const state = (page, s, timeout = 30_000) =>
  page.waitForFunction((want) => window.demo.call.state === want, s, { timeout });
const inCall = (page) =>
  page.waitForFunction(() => window.demo.call.state === 'in-call' && window.demo.call.remotes >= 1, null, { timeout: 30_000 });

// Every call_ringing a page hears from someone else, and every call that ended, by call id.
async function recordRings(page) {
  await page.evaluate(() => {
    window.__rings = [];
    window.__over = [];
    window.demo.chat.addEventListener('call_ringing', (e) => {
      if (e.m.from !== window.demo.user && !window.__rings.includes(e.m.call)) window.__rings.push(e.m.call);
    });
    for (const type of ['call_cancelled', 'call_declined', 'call_missed', 'call_ended']) {
      window.demo.chat.addEventListener(type, (e) => window.__over.push(e.m.call));
    }
  });
}

// Bytes of the other side's audio this page has received, or of its own it has sent.
const audioIn = (page) => page.evaluate(async () => {
  const lk = window.demo.call.lk;
  let bytes = 0;
  for (const p of lk?.remoteParticipants.values() ?? []) {
    const t = p.getTrackPublication('microphone')?.track;
    if (!t?.receiver) continue;
    (await t.receiver.getStats()).forEach((r) => { if (r.type === 'inbound-rtp' && r.kind === 'audio') bytes += r.bytesReceived; });
  }
  return bytes;
});

async function audioFlows(listener) {
  const before = await audioIn(listener);
  await expect.poll(() => audioIn(listener), { timeout: 20_000 }).toBeGreaterThan(before + 2000);
}

const micTrackId = (page) => page.evaluate(() =>
  window.demo.call.lk?.localParticipant.getTrackPublication('microphone')?.track?.mediaStreamTrack.id ?? null);

test.beforeAll(async ({ browser: b }) => { browser = b; });
test.afterAll(async () => {
  for (const page of Object.values(pages)) await page.context().close();
});

test('a call cancelled and rung again is answered', async () => {
  const bob = await open('bob');
  const carol = await open('carol');
  await tab(bob, 'calls');
  await tab(carol, 'videos');
  await bob.locator('#contacts button[data-call="carol"]').click();
  await expect(carol.locator('#ring')).toBeVisible({ timeout: 20_000 });
  const first = await carol.evaluate(() => window.demo.call.ringing?.call);
  await bob.locator('#hangup').click();
  await expect(carol.locator('#ring')).toBeHidden({ timeout: 20_000 });
  await state(bob, 'idle');

  await bob.locator('#contacts button[data-call="carol"]').click();
  await carol.waitForFunction((old) => window.demo.call.ringing && window.demo.call.ringing.call !== old, first, { timeout: 20_000 });
  await expect(carol.locator('#ring')).toBeVisible();
  await carol.locator('#answer').click();
  for (const page of [bob, carol]) await inCall(page);
  const call = await bob.evaluate(() => window.demo.call.callId);
  expect(await carol.evaluate(() => window.demo.call.callId)).toBe(call);
  await carol.locator('#hangup').click();
  await state(bob, 'idle', 40_000);
  await state(carol, 'idle');
});

test('answering a ring whose cancel never arrived ends cleanly and rings nobody back', async () => {
  const alice = await open('alice');
  const carol = await open('carol');
  await tab(alice, 'calls');
  await tab(carol, 'videos');
  await recordRings(alice);
  await alice.locator('#contacts button[data-call="carol"]').click();
  await expect(carol.locator('#ring')).toBeVisible({ timeout: 20_000 });

  // Carol's chat socket goes away (as when chat closes it for an expired token) and stays away
  // while alice gives up: the cancel reaches no socket of hers.
  await carol.evaluate(() => {
    const chat = window.demo.chat;
    chat.connect = () => { window.__reconnect = true; };
    chat.ws.close();
  });
  await carol.waitForFunction(() => window.demo.chat.open === false && window.__reconnect === true);
  await alice.locator('#hangup').click();
  await state(alice, 'idle');
  await carol.evaluate(() => {
    const chat = window.demo.chat;
    delete chat.connect;
    chat.connect();
  });
  await carol.waitForFunction(() => window.demo.chat.open === true);

  // Carol picks up the ring her page still shows: she is told the call ended, and is not left
  // waiting in a call nobody else is in.
  await expect(carol.locator('#ring')).toBeVisible();
  const since = await carol.evaluate(() => Date.now());
  await carol.locator('#answer').click();
  await carol.waitForFunction((t) => window.demo.events.some((e) => e.at >= t && e.kind === 'call_ended_here'), since, { timeout: 30_000 });
  await state(carol, 'idle');
  await expect(carol.locator('#call-panel')).toBeHidden();
  await expect(carol.locator('#ring')).toBeHidden();
  await expect(carol.locator('#toast')).toContainText('ended before you answered');

  // Nothing of that answer is left ringing: alice's next call rings carol, as a new call of hers.
  await alice.locator('#contacts button[data-call="carol"]').click();
  await carol.waitForFunction(() => window.demo.call.ringing?.from === 'alice', null, { timeout: 20_000 });
  await alice.waitForFunction(() => window.demo.call.state === 'calling');
  const { rings, over } = await alice.evaluate(() => ({ rings: window.__rings, over: window.__over }));
  if (await carol.evaluate(() => window.demo.call.answerField)) {
    // Chat refused the late answer (no_call): it rang nobody.
    expect(rings).toEqual([]);
  } else {
    // A chat without `answer` rang alice; carol's page gave that up at once.
    test.info().annotations.push({ type: 'chat', description: 'chat without `answer`: the late answer rang back and was cancelled' });
    for (const id of rings) expect(over).toContain(id);
  }
  await expect(alice.locator('#ring')).toBeHidden();
  await alice.locator('#hangup').click();
  await expect(carol.locator('#ring')).toBeHidden({ timeout: 20_000 });
  await state(alice, 'idle');
});

test('the microphone comes back after the system takes it away', async () => {
  const alice = await open('alice');
  const bob = await open('bob');
  await tab(alice, 'calls');
  await tab(bob, 'calls');
  await alice.locator('#contacts button[data-call="bob"]').click();
  await expect(bob.locator('#ring')).toBeVisible({ timeout: 20_000 });
  await bob.locator('#answer').click();
  for (const page of [alice, bob]) await inCall(page);
  await audioFlows(bob);
  await expect(alice.locator('#toggle-mic')).toHaveText('Mute');

  // 1. The track ends (the system stopped the capture).
  let before = await micTrackId(alice);
  await alice.evaluate(() => {
    const t = window.demo.call.lk.localParticipant.getTrackPublication('microphone').track.mediaStreamTrack;
    t.stop();
    t.dispatchEvent(new Event('ended'));
  });
  await alice.waitForFunction((old) => window.demo.call.mic?.live === true && window.demo.call.mic.track !== old, before, { timeout: 20_000 });
  await audioFlows(bob);
  await expect(alice.locator('#toggle-mic')).toHaveText('Mute');

  // 2. The track stays muted by the system after the interruption (no unmute ever comes).
  before = await micTrackId(alice);
  await alice.evaluate(() => {
    const t = window.demo.call.lk.localParticipant.getTrackPublication('microphone').track.mediaStreamTrack;
    Object.defineProperty(t, 'muted', { configurable: true, get: () => true });
    t.dispatchEvent(new Event('mute'));
  });
  await alice.waitForFunction((old) => window.demo.call.mic?.live === true && window.demo.call.mic.track !== old, before, { timeout: 20_000 });
  await audioFlows(bob);

  // 3. The track ends while the system still holds the microphone: the first attempts fail,
  // the page says so, and the microphone comes back once it is free, unmuted as it was.
  before = await micTrackId(alice);
  await alice.evaluate(() => {
    const md = navigator.mediaDevices;
    const real = md.getUserMedia.bind(md);
    window.__micHeld = true;
    md.getUserMedia = (c) => (window.__micHeld && c?.audio
      ? Promise.reject(new DOMException('Could not start audio source', 'NotReadableError'))
      : real(c));
    const t = window.demo.call.lk.localParticipant.getTrackPublication('microphone').track.mediaStreamTrack;
    t.stop();
    t.dispatchEvent(new Event('ended'));
  });
  await alice.waitForFunction(() => window.demo.call.mic?.interrupted === true, null, { timeout: 20_000 });
  await expect(alice.locator('#mic-note')).toBeVisible();
  await alice.evaluate(() => { window.__micHeld = false; window.dispatchEvent(new Event('focus')); });
  await alice.waitForFunction((old) => window.demo.call.mic?.live === true && window.demo.call.mic.track !== old && !window.demo.call.mic.interrupted, before, { timeout: 20_000 });
  await expect(alice.locator('#mic-note')).toBeHidden();
  await audioFlows(bob);
  await expect(alice.locator('#toggle-mic')).toHaveText('Mute');

  // 4. The button follows the microphone: mute, then unmute, and audio flows again.
  await alice.locator('#toggle-mic').click();
  await alice.waitForFunction(() => window.demo.call.mic?.enabled === false);
  await expect(alice.locator('#toggle-mic')).toHaveText('Unmute');
  await alice.locator('#toggle-mic').click();
  await alice.waitForFunction(() => window.demo.call.mic?.enabled === true && window.demo.call.mic.live === true);
  await expect(alice.locator('#toggle-mic')).toHaveText('Mute');
  await audioFlows(bob);

  await alice.locator('#hangup').click();
  await state(bob, 'idle', 40_000);
});

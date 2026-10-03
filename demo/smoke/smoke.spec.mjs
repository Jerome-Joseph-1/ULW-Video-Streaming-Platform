// Every tab of the demo page, end to end, against a running demo stack (run.sh starts one):
// two or three users, each in a browser context of their own, with Chrome's fake camera and
// microphone. What a build does not serve yet (group calls before feat/group-calls, going live
// before feat/live-publish) is checked to say so on the page, and annotated as skipped.
import { test, expect } from '@playwright/test';

const ROOM = {
  aliceBob: '10000000-0000-4000-8000-000000000001',
  encrypted: '30000000-0000-4000-8000-000000000001',
};

test.describe.configure({ mode: 'serial' });

let browser;
const pages = {};

async function open(user) {
  const context = await browser.newContext({ permissions: ['camera', 'microphone'] });
  const page = await context.newPage();
  page.on('console', (m) => { if (m.type() === 'error') console.log(`[${user}] ${m.text()}`); });
  page.on('pageerror', (e) => console.log(`[${user}] page error: ${e.message}`));
  await page.goto(`/?user=${user}`);
  await page.waitForFunction(() => window.demo?.ready === true);
  await page.waitForFunction(() => window.demo.chat.open === true);
  pages[user] = page;
  return page;
}

const tab = (page, name) => page.locator(`#tabs button[data-tab="${name}"]`).click();

test.beforeAll(async ({ browser: b }) => {
  browser = b;
  await open('alice');
  await open('bob');
});

test.afterAll(async () => {
  for (const page of Object.values(pages)) await page.context().close();
});

test('upload, transcode and play a video', async () => {
  const alice = pages.alice;
  await tab(alice, 'videos');
  await alice.locator('#make-clip').click();
  await expect(alice.locator('#upload')).toBeEnabled({ timeout: 30_000 });
  await alice.locator('#upload').click();
  await expect(alice.locator('#upload-status')).toContainText('transcoding', { timeout: 60_000 });
  const id = await alice.evaluate(() => window.demo.events.find((e) => e.kind === 'upload_committed').video);
  const row = alice.locator(`#videos li[data-video="${id}"]`);
  await expect(row).toHaveAttribute('data-state', /ready|failed/, { timeout: 4 * 60_000 });
  await expect(row).toHaveAttribute('data-state', 'ready', { timeout: 1000 });
  await row.getByRole('button', { name: 'Play' }).click();
  await alice.waitForFunction((v) => window.demo.events.some((e) => e.kind === 'vod_playing' && e.id === v), id, { timeout: 60_000 });
  await alice.waitForFunction(() => window.demo.player.video.currentTime > 1, null, { timeout: 30_000 });
  // The same video is not bob's: the gateway answers 404 for anyone but the owner.
  const status = await pages.bob.evaluate(async (v) => (await window.demo.api?.('GET', `/api/v1/videos/${v}`))?.status
    ?? (await fetch(`/api/v1/videos/${v}`)).status, id);
  expect([401, 404]).toContain(status);
});

test('chat between two users, with presence', async () => {
  const { alice, bob } = pages;
  await tab(alice, 'chat');
  await tab(bob, 'chat');
  await expect(alice.locator('#people [data-presence="bob"]')).toHaveClass(/online/);
  await alice.locator(`#rooms li[data-room="${ROOM.aliceBob}"]`).click();
  await bob.locator(`#rooms li[data-room="${ROOM.aliceBob}"]`).click();
  const text = `hello bob ${Date.now()}`;
  await alice.locator('#message').fill(text);
  await alice.locator('#message').press('Enter');
  await expect(bob.locator('#messages')).toContainText(text);
  await expect(alice.locator('#messages')).toContainText(text);
});

test('end-to-end encrypted chat: the server holds only ciphertext', async () => {
  const { alice, bob } = pages;
  await alice.locator(`#rooms li[data-room="${ROOM.encrypted}"]`).click();
  await bob.locator(`#rooms li[data-room="${ROOM.encrypted}"]`).click();
  // Both devices in the group (MLS: alice starts it, bob asks, alice adds him and he joins).
  for (const page of [alice, bob]) {
    await page.waitForFunction((room) => window.demo.e2ee?.[room]?.().complete === true, ROOM.encrypted, { timeout: 60_000 });
  }
  const label = await alice.locator('#e2ee-banner').textContent();
  test.info().annotations.push({ type: 'e2ee', description: label.includes('MLS (RFC 9420)') ? 'MLS (OpenMLS in WebAssembly)' : 'the stand-in cipher (no MLS client in this build)' });
  const secret = `the launch code is ${Date.now()}`;
  await alice.locator('#message').fill(secret);
  await alice.locator('#message').press('Enter');
  await expect(bob.locator('#messages')).toContainText(secret);
  const stored = await bob.evaluate((room) => window.demo.roomText(room), ROOM.encrypted);
  const mine = stored.find((m) => m.text === secret);
  expect(mine.sender).toBe('alice');
  // The body chat stored and relayed: no plaintext in it.
  const body = Buffer.from(mine.raw, 'base64url');
  expect(body.toString('latin1')).not.toContain(secret);
  // And the sender's own window shows it too (from what it sent: MLS cannot decrypt its own).
  await expect(alice.locator('#messages')).toContainText(secret);
});

test('a 1:1 call rings, is answered, and ends', async () => {
  const { alice, bob } = pages;
  await tab(alice, 'calls');
  await tab(bob, 'videos');
  await alice.locator('#contacts button[data-call="bob"]').click();
  // Bob rings on whatever tab he is on.
  await expect(bob.locator('#ring')).toBeVisible({ timeout: 20_000 });
  await expect(bob.locator('#ring-title')).toContainText('alice is calling');
  const mode = await bob.evaluate(() => window.demo.call.ringing.mode);
  test.info().annotations.push({ type: 'ring', description: mode === 'server' ? "chat's ring (feat/call-ring)" : "the page's ring (chat without feat/call-ring)" });
  await bob.locator('#answer').click();
  for (const page of [alice, bob]) {
    await page.waitForFunction(() => window.demo.call.state === 'in-call' && window.demo.call.remoteVideo >= 1, null, { timeout: 30_000 });
  }
  await alice.locator('#hangup').click();
  await bob.waitForFunction(() => window.demo.call.state === 'idle', null, { timeout: 40_000 });
  await expect(alice.locator('#call-panel')).toBeHidden();
});

test('a declined call stops ringing for both', async () => {
  const { alice, bob } = pages;
  // The ring's limits (calls.md): a declined caller waits 30 s before ringing that member again;
  // bob calls alice instead.
  await bob.locator('#tabs button[data-tab="calls"]').click();
  await bob.locator('#contacts button[data-call="alice"]').click();
  await expect(alice.locator('#ring')).toBeVisible({ timeout: 20_000 });
  await alice.locator('#decline').click();
  await bob.waitForFunction(() => window.demo.call.state === 'idle', null, { timeout: 20_000 });
  await expect(alice.locator('#ring')).toBeHidden();
});

test('a group call', async () => {
  const { alice, bob } = pages;
  const carol = pages.carol ?? await open('carol');
  for (const page of [alice, bob, carol]) await tab(page, 'calls');
  await alice.locator('#groups button[data-group-call]').first().click();
  const note = alice.locator('#group-note');
  const joined = alice.waitForFunction(() => window.demo.call.state === 'in-call', null, { timeout: 20_000 }).then(() => 'joined');
  const refused = note.filter({ hasText: 'group' }).waitFor({ timeout: 20_000 }).then(() => 'refused');
  if (await Promise.race([joined, refused]) === 'refused') {
    await expect(note).toContainText('feat/group-calls');
    test.info().annotations.push({ type: 'skip', description: "this build's chat has no group calls (feat/group-calls)" });
    return;
  }
  // Joining may ring the others (a group call's ring); answering joins it as the button does.
  for (const page of [bob, carol]) {
    if (await page.locator('#ring').isVisible().catch(() => false) ||
        await page.locator('#ring').waitFor({ state: 'visible', timeout: 3000 }).then(() => true, () => false)) {
      await page.locator('#answer').click();
    } else {
      await page.locator('#groups button[data-group-call]').first().click();
    }
  }
  for (const page of [alice, bob, carol]) {
    await page.waitForFunction(() => window.demo.call.remotes === 2 && window.demo.call.remoteVideo === 2, null, { timeout: 40_000 });
  }
  for (const page of [alice, bob, carol]) await page.locator('#hangup').click();
});

test('go live, watch, and get the recording as a video', async () => {
  const { alice, bob } = pages;
  await tab(alice, 'live');
  await tab(bob, 'live');
  await alice.locator('#go-live').click();
  const status = alice.locator('#live-status');
  await expect(status).toContainText(/LIVE|no stream service|could not|refused/, { timeout: 90_000 });
  if ((await status.textContent()).includes('no stream service')) {
    test.info().annotations.push({ type: 'skip', description: "this build's gateway has no stream service (feat/live-publish, #141)" });
    return;
  }
  await expect(status).toContainText('LIVE');
  const id = await alice.evaluate(() => window.demo.live.id);
  const row = bob.locator(`#streams li[data-stream="${id}"]`);
  await expect(row).toHaveAttribute('data-state', 'live', { timeout: 30_000 });
  await row.getByRole('button', { name: 'Watch' }).click();
  await bob.waitForFunction(() => window.demo.live.viewer?.playing === true, null, { timeout: 90_000 });
  await bob.waitForFunction(() => document.getElementById('live-player').currentTime > 2, null, { timeout: 30_000 });
  await alice.locator('#end-live').click();
  await alice.waitForFunction(() => window.demo.live.videoId, null, { timeout: 4 * 60_000 });
  const video = await alice.evaluate(() => window.demo.live.videoId);
  await tab(alice, 'videos');
  await expect(alice.locator(`#videos li[data-video="${video}"]`)).toHaveAttribute('data-state', 'ready', { timeout: 4 * 60_000 });
});

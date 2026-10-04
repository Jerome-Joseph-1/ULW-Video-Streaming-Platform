// The page: pick a user, then the four tabs. Each window signs in on its own, so two windows
// of one browser can be two users.
import { $, chat, config, demo, directory, el, loadDirectory, oidcConfig, signIn, usesOidc } from './core.js';
import * as oidc from './oidc.js';
import { initVideos } from './videos.js';
import { initChat, syncRooms } from './chat.js';
import { initCalls, refreshCalls } from './calls.js';
import { relist, useRoomsApi } from './rooms.js';
import { initLive } from './live.js';

function showTab(name) {
  for (const b of document.querySelectorAll('#tabs button')) b.classList.toggle('active', b.dataset.tab === name);
  for (const s of document.querySelectorAll('.tab')) s.hidden = s.id !== `tab-${name}`;
  try { sessionStorage.setItem('ulw-demo:tab', name); } catch { /* ignore */ }
}

async function start(who) {
  const user = await signIn(who);
  document.title = `${user} - ULW demo`;
  $('picker').hidden = true;
  $('tabs').hidden = false;
  $('whoami').hidden = false;
  $('me').textContent = user;
  await chat.connect();
  await new Promise((resolve) => (chat.open ? resolve() : chat.addEventListener('open', resolve, { once: true })));
  // The rooms chat lists, where it has the member-list commands; else web/rooms.json.
  if (await useRoomsApi()) {
    chat.addEventListener('member', async () => {
      await relist();
      syncRooms();
      refreshCalls();
    });
  }
  initVideos();
  initChat();
  initCalls();
  initLive();
  let tab = 'videos';
  try { tab = new URLSearchParams(location.search).get('tab') ?? sessionStorage.getItem('ulw-demo:tab') ?? 'videos'; } catch { /* ignore */ }
  showTab(tab);
  demo.ready = true;
}

async function main() {
  await loadDirectory();
  for (const b of document.querySelectorAll('#tabs button')) b.addEventListener('click', () => showTab(b.dataset.tab));
  if (config.label !== undefined) $('label').textContent = config.label;
  if (usesOidc()) return mainOidc();
  $('switch-user').addEventListener('click', () => {
    try { sessionStorage.removeItem('ulw-demo:user'); } catch { /* ignore */ }
    location.href = location.pathname;
  });
  const fromUrl = new URLSearchParams(location.search).get('user');
  let saved = null;
  try { saved = sessionStorage.getItem('ulw-demo:user'); } catch { /* ignore */ }
  const user = [fromUrl, saved].find((u) => u && directory.users.includes(u));
  if (user) return start(user);
  for (const u of directory.users) {
    $('user-buttons').append(el('button', { class: 'primary', dataset: { user: u }, onclick: () => start(u) }, u));
  }
}

// A real identity provider: its sign-in page decides who this window is (oidc.js).
async function mainOidc() {
  $('switch-user').textContent = 'sign out';
  $('switch-user').addEventListener('click', () => oidc.logout(oidcConfig));
  $('picker-title').textContent = 'Sign in';
  $('picker-note').textContent = 'Sign in to upload and watch, chat, call and go live. Another user? Use another browser or a private window.';
  if (await oidc.complete(oidcConfig) || oidc.current()) return start();
  $('user-buttons').append(el('button', { class: 'primary', id: 'sign-in', onclick: () => oidc.login(oidcConfig) }, 'Sign in'));
}

main().catch((e) => {
  document.body.prepend(el('pre', { class: 'card' }, `The demo did not start: ${e.message}`));
  console.error(e);
});

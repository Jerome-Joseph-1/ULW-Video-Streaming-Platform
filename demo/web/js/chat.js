// Chat: the user's rooms (direct and group chats, and the encrypted room), messages with
// history, and presence (docs/integration/chat.md). Bodies are this page's JSON envelope:
//   {t:'text', text}          a message
// The encrypted room's bodies belong to e2ee.js (MLS messages, or the stand-in's envelope).
//   {t:'ring' | 'live', ...}  the page's own signals, handed to calls.js and live.js
import { $, chat, decodeBody, demo, directory, el, log, myRooms, presenceDot, roomById, sendBody, session, store } from './core.js';
import * as e2ee from './e2ee.js';

const rooms = new Map(); // id -> { meta, messages: Map(seq -> view), loading, buffer, unread, session }
let current = null;
const appListeners = new Set();
demo.rooms = rooms;

// calls.js and live.js hear the page's own signals: { room, sender, seq, body, live }. `live`
// is false for what history brought back.
export function onAppMessage(fn) { appListeners.add(fn); }

export function initChat() {
  for (const meta of myRooms()) {
    rooms.set(meta.id, { meta, messages: new Map(), loading: false, buffer: [], unread: 0, session: null, loaded: false });
  }
  for (const user of directory.users) if (user !== session.user) chat.watch(user);
  renderRooms();
  renderPeople();

  chat.addEventListener('open', () => {
    for (const r of rooms.values()) {
      if (!r.loaded) { r.loading = true; r.buffer = []; }
      chat.join(r.meta.id, r.meta.kind);
    }
  });
  chat.addEventListener('joined', (e) => {
    const r = rooms.get(e.m.room);
    if (!r) return;
    if (!r.loaded) chat.send({ type: 'history', room: e.m.room, limit: 100 });
  });
  chat.addEventListener('message', (e) => {
    const r = rooms.get(e.m.room);
    if (!r) return;
    if (r.loading) r.buffer.push(e.m);
    else queue(r, () => take(r, e.m, true));
  });
  chat.addEventListener('history', (e) => {
    const r = rooms.get(e.m.room);
    if (!r || !r.loading) return;
    const lowest = Math.min(...r.buffer.map((m) => m.seq));
    if (e.m.count === 100 && lowest > 1) {
      chat.send({ type: 'history', room: e.m.room, before: lowest, limit: 100 });
      return;
    }
    queue(r, () => finishLoading(r));
  });
  chat.addEventListener('error', (e) => {
    if (e.m.room && rooms.has(e.m.room) && e.m.reason !== 'not_callable') {
      system(rooms.get(e.m.room), `chat said: ${e.m.reason}`);
    }
  });

  $('composer').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const text = $('message').value.trim();
    if (!text || !current) return;
    const r = rooms.get(current);
    if (r.meta.e2ee) {
      if (!r.session || !r.session.state().canSend) { system(r, `not yet: ${r.session?.state().detail ?? 'loading'}`); return; }
      // The echo of this id is shown as this text: an MLS member cannot decrypt its own message.
      const sent = store.get(`sent:${r.meta.id}`, {});
      const id = await r.session.send(text);
      sent[id] = text;
      store.set(`sent:${r.meta.id}`, sent);
    } else {
      sendBody(r.meta.id, { t: 'text', text });
    }
    $('message').value = '';
  });
  $('show-raw').addEventListener('change', () => renderMessages());
  $('reset-e2ee').addEventListener('click', () => {
    e2ee.resetDevice();
    location.reload();
  });
}

// One room's messages are handled one at a time, in order: decryption is asynchronous.
function queue(r, fn) {
  r.chain = (r.chain ?? Promise.resolve()).then(fn).catch((e) => console.error(e));
}

async function finishLoading(r) {
  const history = r.buffer.splice(0).sort((a, b) => a.seq - b.seq);
  const seen = new Set();
  r.loading = false;
  r.loaded = true;
  if (r.meta.e2ee && !r.session) {
    r.session = await e2ee.openSession({ user: session.user, room: r.meta.id, members: r.meta.members,
      post: (id, body) => chat.send({ type: 'send', room: r.meta.id, id, body }) });
    demo.e2ee = demo.e2ee ?? {};
    demo.e2ee[r.meta.id] = () => r.session.state();
  }
  for (const m of history) {
    if (seen.has(m.seq)) continue;
    seen.add(m.seq);
    await take(r, m, false);
  }
  // Whatever arrived live while history was being read.
  for (const m of r.buffer.splice(0).sort((a, b) => a.seq - b.seq)) await take(r, m, true);
  if (r.session) await r.session.ready();
  log('room_loaded', { room: r.meta.id, messages: r.messages.size });
  if (current === r.meta.id) renderMessages();
}

async function take(r, m, live) {
  if (r.messages.has(m.seq)) return;
  const view = { seq: m.seq, sender: m.sender, raw: m.body };
  r.messages.set(m.seq, view);
  if (r.meta.e2ee) {
    const mine = m.sender === session.user ? store.get(`sent:${r.meta.id}`, {})[m.id] : undefined;
    const out = r.session ? await r.session.receive(m) : null;
    if (mine !== undefined) { view.text = mine; view.decrypted = true; }
    else if (out?.text !== undefined) { view.text = out.text; view.decrypted = true; view.warning = out.warning; }
    else if (out?.note) { view.system = out.note; }
    else if (out?.error) { view.text = `(${out.error})`; view.locked = true; }
    else { view.hidden = true; }
  } else {
    let body;
    try { body = decodeBody(m.body); } catch { body = { t: 'text', text: '(unreadable body)' }; }
    view.body = body;
    if (body.t === 'text') {
      view.text = body.text;
    } else if (body.t === 'e2ee') {
      view.text = '(encrypted message)';
      view.locked = true;
    } else {
      view.hidden = true;
      for (const fn of appListeners) fn({ room: r.meta.id, sender: m.sender, seq: m.seq, body, live });
      if (body.t === 'ring' && body.what === 'ring') view.system = `${m.sender} called`;
      if (body.t === 'live' && body.what === 'started') view.system = `${m.sender} went live`;
      if (view.system) view.hidden = false;
    }
  }
  if (r.meta.e2ee && current === r.meta.id) updateBanner(r);
  if (live && !view.hidden && m.sender !== session.user && current !== r.meta.id) {
    r.unread += 1;
    renderRooms();
  }
  if (current === r.meta.id && !r.loading) appendMessage(view);
}

function system(r, text) {
  if (current === r.meta.id) $('messages').append(el('li', { class: 'system' }, text));
}

function renderRooms() {
  const list = $('rooms');
  list.replaceChildren();
  let unread = 0;
  for (const r of rooms.values()) {
    unread += r.unread;
    const others = r.meta.members.filter((m) => m !== session.user);
    list.append(el('li', { class: `clickable ${current === r.meta.id ? 'selected' : ''}`, dataset: { room: r.meta.id },
      onclick: () => select(r.meta.id) },
    el('span', { class: 'grow' }, `${r.meta.e2ee ? '\u{1F512} ' : ''}${r.meta.kind === 'direct' ? others[0] : r.meta.name}`),
    r.unread ? el('span', { class: 'badge' }, String(r.unread)) : null));
  }
  $('chat-unread').hidden = unread === 0;
  $('chat-unread').textContent = String(unread);
}

function renderPeople() {
  const list = $('people');
  list.replaceChildren();
  for (const user of directory.users) {
    if (user === session.user) continue;
    list.append(el('li', {}, el('span', { class: 'grow' }, user), presenceDot(user)));
  }
}

export function select(id) {
  current = id;
  const r = rooms.get(id);
  r.unread = 0;
  renderRooms();
  $('room-title').textContent = r.meta.name;
  $('composer').hidden = false;
  $('raw-toggle').hidden = !r.meta.e2ee;
  $('e2ee-banner').hidden = !r.meta.e2ee;
  renderMessages();
  $('message').focus();
}

function renderMessages() {
  const r = rooms.get(current);
  if (!r) return;
  const list = $('messages');
  list.replaceChildren();
  if (r.loading) list.append(el('li', { class: 'system' }, 'loading history...'));
  for (const view of [...r.messages.values()].sort((a, b) => a.seq - b.seq)) appendMessage(view, false);
  updateBanner(r);
  list.scrollTop = list.scrollHeight;
}

function updateBanner(r) {
  if (!r.meta.e2ee) return;
  const state = r.session?.state();
  $('e2ee-banner').textContent = `End-to-end encrypted: ${r.session?.label ?? 'loading the encryption'}. ` +
    `The chat server only stores and relays ciphertext. ${state ? state.detail : ''}`;
}

function appendMessage(view, scroll = true) {
  const list = $('messages');
  const r = rooms.get(current);
  if (view.hidden) return;
  if (view.system) {
    list.append(el('li', { class: 'system' }, view.system));
  } else {
    const showRaw = r.meta.e2ee && $('show-raw').checked;
    list.append(el('li', { class: view.sender === session.user ? 'mine' : '', dataset: { seq: view.seq } },
      el('div', { class: 'who' }, `${view.sender}${view.decrypted ? ' \u{1F512}' : ''}${view.warning ? ` (${view.warning})` : ''}`),
      el('div', { class: 'text' }, view.text),
      showRaw ? el('span', { class: 'raw' }, `seq ${view.seq}, stored body: ${view.raw.slice(0, 160)}${view.raw.length > 160 ? '...' : ''}`) : null));
  }
  if (r.meta.e2ee) updateBanner(r);
  if (scroll) list.scrollTop = list.scrollHeight;
}

// The text of each message in a room, for the smoke test.
demo.roomText = (id) => [...(rooms.get(id)?.messages.values() ?? [])].filter((v) => v.text !== undefined).map((v) => ({ sender: v.sender, text: v.text, raw: v.raw }));
export { roomById };

// What every tab shares: the signed-in user and their token, the gateway API, the chat socket,
// and small UI helpers. window.demo exposes the state the smoke test reads.

export const demo = (window.demo = { events: [] });
export const $ = (id) => document.getElementById(id);

export function log(kind, detail = {}) {
  demo.events.push({ at: Date.now(), kind, ...detail });
  if (demo.events.length > 500) demo.events.shift();
}

let toastTimer;
export function toast(text) {
  const el = $('toast');
  el.textContent = text;
  el.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('show'), 4000);
}

export function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === 'class') node.className = v;
    else if (k === 'dataset') Object.assign(node.dataset, v);
    else if (k.startsWith('on')) node.addEventListener(k.slice(2), v);
    else if (v !== undefined && v !== null) node.setAttribute(k, v);
  }
  for (const child of children.flat()) {
    if (child !== null && child !== undefined) node.append(child);
  }
  return node;
}

// Per-user browser storage. Windows of one browser share localStorage, so every key names the
// user; the token is per window (sessionStorage).
export const store = {
  get(key, fallback) {
    try { return JSON.parse(localStorage.getItem(`ulw-demo:${session.user}:${key}`)) ?? fallback; } catch { return fallback; }
  },
  set(key, value) {
    try { localStorage.setItem(`ulw-demo:${session.user}:${key}`, JSON.stringify(value)); } catch { /* private window */ }
  },
};

export const session = { user: null, token: null, expiresAt: 0 };

// Where the page finds the gateway, chat and the token issuer: window.ULW_CONFIG, from config.js
// (the web image writes it at start from ULW_WEB_* variables, build/web-config.sh). Each empty
// or missing value means this page's own origin and the demo's paths, as before.
export const config = window.ULW_CONFIG ?? {};
// A gateway path (/api/v1/...) on the configured API base, or on this origin.
export const apiUrl = (path) =>
  /^https?:/.test(path) ? path : `${(config.apiBase ?? '').replace(/\/$/, '')}${path}`;
export const apiOrigin = new URL(apiUrl('/'), location.href).origin;

export async function signIn(user) {
  const r = await fetch(`${config.tokenUrl || '/auth/token'}?sub=${encodeURIComponent(user)}`, { method: 'POST' });
  if (!r.ok) throw new Error(`token for ${user}: ${r.status}`);
  const body = await r.json();
  session.user = user;
  session.token = body.token;
  session.expiresAt = Date.now() + body.expires_in * 1000;
  try { sessionStorage.setItem('ulw-demo:user', user); } catch { /* ignore */ }
  demo.user = user;
  return body.token;
}

export async function token() {
  if (Date.now() > session.expiresAt - 5 * 60_000) await signIn(session.user);
  return session.token;
}

// The gateway, on this page's own origin (the web proxy's /api), with the bearer token: the
// demo's users share one browser, so the cookie is not used.
export async function api(method, path, { json, body, headers = {} } = {}) {
  const init = { method, headers: { authorization: `Bearer ${await token()}`, ...headers } };
  if (json !== undefined) {
    init.body = JSON.stringify(json);
    init.headers['content-type'] = 'application/json';
  } else if (body !== undefined) {
    init.body = body;
  }
  const response = await fetch(apiUrl(path), init);
  let data = null;
  if ((response.headers.get('content-type') ?? '').startsWith('application/json')) {
    data = await response.json().catch(() => null);
  }
  return { status: response.status, ok: response.ok, data, headers: response.headers };
}

// A LiveKit URL as chat or the gateway names it (the deployment's LIVEKIT_CLIENT_URL, which
// says localhost) put on whatever host this page was opened on, so 127.0.0.1 works too.
export function onThisHost(url) {
  const u = new URL(url);
  if (u.hostname === 'localhost' || u.hostname === '127.0.0.1') {
    u.host = location.host;
    if (u.protocol === 'ws:' && location.protocol === 'https:') u.protocol = 'wss:';
  }
  return u.href.replace(/\/$/, '');
}

export const uuid = () => crypto.randomUUID();
export const utf8 = new TextEncoder();
export const fromUtf8 = new TextDecoder();
export const b64url = {
  encode(bytes) {
    let s = '';
    for (const b of bytes) s += String.fromCharCode(b);
    return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  },
  decode(text) {
    const s = atob(text.replace(/-/g, '+').replace(/_/g, '/'));
    return Uint8Array.from(s, (c) => c.charCodeAt(0));
  },
};

// Chat's WebSocket (docs/integration/chat.md). One per window. The web proxy turns ?token=
// into the bearer header (nginx/default.conf). Reconnects with backoff, then rejoins every
// room after the last seq it holds and watches everyone again.
class ChatSocket extends EventTarget {
  constructor() {
    super();
    this.ws = null;
    this.rooms = new Map(); // room -> { kind, lastSeq, joined }
    this.watching = new Set();
    this.pending = [];
    this.delay = 500;
    this.open = false;
  }

  async connect() {
    const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
    const base = config.chatUrl || `${scheme}://${location.host}/rt`;
    const ws = new WebSocket(`${base}?token=${encodeURIComponent(await token())}`);
    this.ws = ws;
    ws.onopen = () => {
      this.open = true;
      this.delay = 500;
      $('conn-dot').classList.add('online');
      log('chat_open');
      for (const [room, r] of this.rooms) this.join(room, r.kind, { rejoin: true });
      for (const user of this.watching) this.send({ type: 'watch', user });
      for (const m of this.pending.splice(0)) this.send(m);
      this.dispatchEvent(new Event('open'));
    };
    ws.onmessage = (e) => {
      let m;
      try { m = JSON.parse(e.data); } catch { return; }
      if (m.type === 'joined' && this.rooms.has(m.room)) this.rooms.get(m.room).joined = true;
      if (m.type === 'message' && this.rooms.has(m.room)) {
        const r = this.rooms.get(m.room);
        r.lastSeq = Math.max(r.lastSeq ?? 0, m.seq);
      }
      this.dispatchEvent(Object.assign(new Event(m.type), { m }));
      this.dispatchEvent(Object.assign(new Event('any'), { m }));
    };
    ws.onclose = (e) => {
      if (this.ws !== ws) return;
      this.open = false;
      $('conn-dot').classList.remove('online');
      log('chat_closed', { code: e.code });
      for (const r of this.rooms.values()) r.joined = false;
      this.dispatchEvent(new Event('close'));
      setTimeout(() => this.connect(), this.delay);
      this.delay = Math.min(this.delay * 2, 8000);
    };
  }

  send(m) {
    if (this.open && this.ws.readyState === WebSocket.OPEN) this.ws.send(JSON.stringify(m));
    else this.pending.push(m);
  }

  join(room, kind, { rejoin = false } = {}) {
    const r = this.rooms.get(room) ?? { kind, lastSeq: 0, joined: false };
    this.rooms.set(room, r);
    const m = { type: 'join', room };
    if (kind) m.kind = kind;
    if (rejoin && r.lastSeq) m.after = r.lastSeq;
    if (this.open) this.ws.send(JSON.stringify(m));
  }

  watch(user) {
    this.watching.add(user);
    if (this.open) this.send({ type: 'watch', user });
  }

  // The next frame matching `test`, or null after `ms`.
  next(test, ms = 10_000) {
    return new Promise((resolve) => {
      const on = (e) => {
        if (test(e.m)) { this.removeEventListener('any', on); clearTimeout(timer); resolve(e.m); }
      };
      const timer = setTimeout(() => { this.removeEventListener('any', on); resolve(null); }, ms);
      this.addEventListener('any', on);
    });
  }

  close() {
    const ws = this.ws;
    this.ws = null;
    ws?.close();
  }
}

export const chat = new ChatSocket();
demo.api = api;
demo.chat = chat;

// The demo's rooms and who is in them (rooms.json; db/seed.sql lists the same members).
export const directory = { users: [], rooms: [] };
demo.directory = directory;
export async function loadDirectory() {
  const r = await fetch('rooms.json');
  Object.assign(directory, await r.json());
  return directory;
}
export const myRooms = () => directory.rooms.filter((r) => r.members.includes(session.user));
export const directRoomWith = (user) => myRooms().find((r) => r.kind === 'direct' && r.members.includes(user));
export const roomById = (id) => directory.rooms.find((r) => r.id === id);

// Presence, as chat reports it.
export const presence = new Map();
chat.addEventListener('watching', (e) => setPresence(e.m.user, e.m.status));
chat.addEventListener('presence', (e) => setPresence(e.m.user, e.m.status));
function setPresence(user, status) {
  presence.set(user, status);
  for (const dot of document.querySelectorAll(`[data-presence="${user}"]`)) {
    dot.classList.toggle('online', status === 'online');
    dot.title = `${user} is ${status}`;
  }
}
export function presenceDot(user) {
  const dot = el('span', { class: 'dot', dataset: { presence: user } });
  if (presence.get(user) === 'online') dot.classList.add('online');
  return dot;
}

// Message bodies are JSON, base64url (chat carries any bytes). Plain text from another client
// shows as itself.
export function encodeBody(obj) {
  return b64url.encode(utf8.encode(JSON.stringify(obj)));
}
export function decodeBody(body) {
  const text = fromUtf8.decode(b64url.decode(body));
  try {
    const obj = JSON.parse(text);
    if (obj && typeof obj === 'object' && typeof obj.t === 'string') return obj;
  } catch { /* plain text */ }
  return { t: 'text', text };
}
export function sendBody(room, obj) {
  const id = uuid();
  chat.send({ type: 'send', room, id, body: encodeBody(obj) });
  return id;
}

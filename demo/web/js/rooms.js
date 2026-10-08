// The user's rooms from chat's member-list commands (feat/chat-rooms-api; docs/integration/
// chat.md, "Changing member lists"): each user opens a direct chat with every other demo user,
// and alice (the first demo user) creates the demo's two groups, everyone's "team" and the
// encrypted room of alice and bob. A chat without the commands (an unknown `type` is refused)
// leaves the page on web/rooms.json, whose rooms db/seed.sql lists.
import { chat, demo, directory, log, session } from './core.js';

// The demo's groups, by their create_group request id.
const GROUPS = {
  'demo-team': { name: 'team' },
  'demo-encrypted': { name: 'encrypted (alice + bob)', e2ee: true, users: ['bob'] },
};

const encoder = new TextEncoder();

// A named room's id, as chat derives it: SHA-256 over the name, its first byte replaced by
// the kind (3 direct, 4 group), with the version 8 and variant bits set. Used only to label
// the demo's groups on every member's page; the rooms themselves come from chat.
async function namedRoom(kind, text) {
  const b = new Uint8Array(await crypto.subtle.digest('SHA-256', encoder.encode(text))).slice(0, 16);
  b[0] = kind;
  b[6] = (b[6] & 0x0f) | 0x80;
  b[8] = (b[8] & 0x3f) | 0x80;
  const h = [...b].map((x) => x.toString(16).padStart(2, '0')).join('');
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}

const answered = (test, ms = 8000) => chat.next(test, ms);

let known = {};

// true when chat has the commands; then directory.rooms is what chat lists.
export async function useRoomsApi() {
  chat.send({ type: 'rooms', limit: 100 });
  const first = await answered((m) => m.type === 'rooms' || (m.type === 'error' && !m.room && m.reason === 'malformed'), 8000);
  if (first?.type !== 'rooms') {
    log('rooms_api', { available: false });
    return false;
  }
  directory.api = true;
  demo.roomsApi = true;
  const me = session.user;
  const creator = directory.users[0];
  for (const [id, group] of Object.entries(GROUPS)) {
    known[await namedRoom(4, `ulw group chat\n${creator}\n${id}`)] = group;
  }
  for (const user of directory.users) {
    if (user === me) continue;
    chat.send({ type: 'open_direct', user });
    await answered((m) => (m.type === 'direct' || m.type === 'error') && m.user === user);
  }
  if (me === creator) {
    for (const [id, group] of Object.entries(GROUPS)) {
      chat.send({ type: 'create_group', id, users: group.users ?? directory.users.filter((u) => u !== me) });
      await answered((m) => (m.type === 'group' || m.type === 'error') && m.id === id);
    }
  }
  await relist();
  log('rooms_api', { available: true, rooms: directory.rooms.length });
  return true;
}

// Reads the user's rooms, and each group's members (its admin first: in the encrypted room,
// the one who starts the MLS group).
export async function relist() {
  const listed = [];
  let after;
  for (;;) {
    chat.send({ type: 'rooms', limit: 100, ...(after ? { after } : {}) });
    const page = await answered((m) => m.type === 'rooms');
    if (!page) break;
    listed.push(...page.rooms);
    if (!page.more || page.rooms.length === 0) break;
    after = page.rooms.at(-1).room;
  }
  const rooms = [];
  for (const r of listed) {
    if (r.kind === 'direct') {
      rooms.push({ id: r.room, kind: 'direct', name: `${session.user} + ${r.peer}`, members: [session.user, r.peer] });
    } else if (r.kind === 'group') {
      chat.send({ type: 'members', room: r.room, limit: 100 });
      const m = await answered((x) => (x.type === 'members' || x.type === 'error') && x.room === r.room);
      const members = (m?.members ?? []).slice()
        .sort((a, b) => (a.role === 'admin' ? 0 : 1) - (b.role === 'admin' ? 0 : 1) || (a.user < b.user ? -1 : 1))
        .map((x) => x.user);
      const group = known[r.room];
      rooms.push({ id: r.room, kind: 'group', name: group?.name ?? `group: ${members.join(', ')}`, members, e2ee: Boolean(group?.e2ee) });
    }
  }
  directory.rooms = rooms;
  return rooms;
}

// Opens (or finds) the direct chat with `user`: the answer names its room.
export async function openDirect(user) {
  chat.send({ type: 'open_direct', user });
  const m = await answered((x) => (x.type === 'direct' || x.type === 'error') && x.user === user);
  return m?.type === 'direct' ? m.room : null;
}

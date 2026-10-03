// Calls: 1:1 calls in a direct chat with an incoming-call ring, and a group call in a group
// chat, through LiveKit with tickets from chat (docs/integration/calls.md).
//
// The ring comes from chat where chat rings (feat/call-ring: a ticket names its `call`, and
// call_ringing, call_answered, ... arrive unasked). A chat without it gives tickets with no
// `call`; then the page rings through the room itself, as calls.md says to: its own
// {t:'ring'} messages, which only this page understands.
import { $, chat, demo, directRoomWith, directory, el, log, myRooms, onThisHost, presenceDot, roomById, sendBody, session, store, toast, uuid } from './core.js';
import { onAppMessage } from './chat.js';

const { Room, RoomEvent, Track } = window.LivekitClient;
let call = null; // { room, callId, peer, role, mode, lk, state, kind }
let ringing = null; // { room, callId, from, mode, expires }
demo.call = { state: 'idle', remotes: 0 };

function device() {
  let id = store.get('device', null);
  if (!id) { id = uuid(); store.set('device', id); }
  return id;
}

export function initCalls() {
  renderContacts();
  renderGroups();
  $('hangup').addEventListener('click', () => hangUp());
  $('end-for-all').addEventListener('click', () => {
    if (call?.kind !== 'group' || !call.callId) return;
    chat.send({ type: 'call_end', room: call.room, call: call.callId });
    endCall('you ended the call for everyone');
  });
  $('answer').addEventListener('click', () => answer());
  $('decline').addEventListener('click', () => decline());
  $('toggle-mic').addEventListener('click', async () => {
    if (!call?.lk) return;
    const on = call.lk.localParticipant.isMicrophoneEnabled;
    await call.lk.localParticipant.setMicrophoneEnabled(!on);
    $('toggle-mic').textContent = on ? 'Unmute' : 'Mute';
  });
  $('toggle-cam').addEventListener('click', async () => {
    if (!call?.lk) return;
    const on = call.lk.localParticipant.isCameraEnabled;
    await call.lk.localParticipant.setCameraEnabled(!on);
    $('toggle-cam').textContent = on ? 'Camera on' : 'Camera off';
    renderTiles();
  });

  // Chat's ring (feat/call-ring).
  chat.addEventListener('call_ringing', (e) => {
    const m = e.m;
    if (m.from === session.user) return;
    if (call && call.callId === m.call) return;
    if (call && call.room === m.room && call.state !== 'ended') {
      // Already in this room's call (a rejoin rang): answer by asking for a ticket.
      return;
    }
    showRing({ room: m.room, callId: m.call, from: m.from, mode: 'server', expires: m.expires_at * 1000 });
  });
  for (const type of ['call_answered', 'call_declined', 'call_cancelled', 'call_missed', 'call_ended', 'call_moved', 'call_left']) {
    chat.addEventListener(type, (e) => onCallEvent(type, e.m));
  }
  // The page's own ring, where chat does not ring.
  onAppMessage(({ room, sender, body, live }) => {
    if (body.t !== 'ring' || !live || sender === session.user) return;
    if (body.what === 'ring' && body.to === session.user && body.expires > Date.now()) {
      showRing({ room, callId: body.call, from: sender, mode: 'app', expires: body.expires });
    } else if (body.what === 'cancel' && ringing?.callId === body.call) {
      stopRing(`${sender} cancelled the call`);
    } else if (call?.callId === body.call) {
      if (body.what === 'decline') endCall(`${sender} declined`);
      if (body.what === 'end') endCall(`${sender} ended the call`);
      if (body.what === 'answer') setState('connecting', `${sender} answered`);
    }
  });
}

// The rooms changed (chat's member-list commands): redraw who can be called.
export function refreshCalls() {
  renderContacts();
  renderGroups();
}

function renderContacts() {
  const list = $('contacts');
  list.replaceChildren();
  for (const user of directory.users) {
    if (user === session.user || !directRoomWith(user)) continue;
    list.append(el('li', {}, presenceDot(user), el('span', { class: 'grow' }, user),
      el('button', { class: 'primary', dataset: { call: user }, onclick: () => startCall(user) }, 'Call')));
  }
}

function renderGroups() {
  const list = $('groups');
  list.replaceChildren();
  for (const r of myRooms()) {
    if (r.kind !== 'group' || r.e2ee) continue;
    list.append(el('li', {}, el('span', { class: 'grow' }, `${r.name} (${r.members.join(', ')})`),
      el('button', { class: 'primary', dataset: { groupCall: r.id }, onclick: () => startGroupCall(r) }, 'Join group call')));
  }
}

// Asks chat for a ticket to the room's call: the answer is a `ticket` or an `error`.
async function ticketFor(room) {
  chat.send({ type: 'call', room, device: device() });
  const m = await chat.next((x) => x.room === room && (x.type === 'ticket' || x.type === 'error'), 15_000);
  if (!m) throw new Error('no answer from chat');
  if (m.type === 'error') throw Object.assign(new Error(m.reason), { reason: m.reason, retry: m.retry_after_ms });
  return m;
}

async function startCall(peer) {
  if (call) { toast('already in a call'); return; }
  const room = directRoomWith(peer).id;
  call = { room, peer, role: 'caller', kind: 'direct', state: 'calling' };
  showPanel(`Calling ${peer}...`);
  let ticket;
  try {
    ticket = await ticketFor(room);
  } catch (e) {
    call = null;
    hidePanel();
    toast(e.reason === 'ring_limited' ? `cannot ring ${peer} again yet (${Math.ceil((e.retry ?? 0) / 1000)} s)` : `call refused: ${e.message}`);
    return;
  }
  if (ticket.call) {
    call.mode = 'server';
    call.callId = ticket.call;
  } else {
    call.mode = 'app';
    call.callId = uuid();
    sendBody(room, { t: 'ring', what: 'ring', call: call.callId, to: peer, expires: Date.now() + 45_000 });
  }
  log('call_started', { mode: call.mode, peer });
  setState('calling', `ringing ${peer} (${call.mode === 'server' ? "chat's ring" : "the page's ring: this build's chat has none"})`);
  await connect(ticket);
  // An app-mode call nobody answers stops after the ring's 45 s, as chat's does.
  const mine = call;
  setTimeout(() => { if (call === mine && call.state === 'calling') hangUp('no answer'); }, 46_000);
}

async function rejoinGroup(why) {
  const c = call;
  if (!c) return;
  setState('connecting', why);
  const old = c.lk;
  c.lk = null;
  await old?.disconnect();
  try {
    const ticket = await ticketFor(c.room);
    if (call !== c) return;
    await connect(ticket);
    setState('in-call', 'in the group call');
  } catch (e) {
    endCall(`could not rejoin: ${e.message}`);
  }
}

async function startGroupCall(r) {
  if (call) { toast('already in a call'); return; }
  call = { room: r.id, role: 'member', kind: 'group', state: 'connecting', mode: 'server' };
  showPanel(`${r.name}: group call`);
  try {
    const ticket = await ticketFor(r.id);
    call.callId = ticket.call ?? null;
    await connect(ticket);
    setState('in-call', 'connected; others join from their Calls tab');
  } catch (e) {
    const note = e.reason === 'not_callable'
      ? 'This build\'s chat has no group calls yet (calls.md: "Group calls are not available"); they arrive with feat/group-calls.'
      : `group call refused: ${e.message}`;
    $('group-note').textContent = note;
    toast(note);
    call = null;
    hidePanel();
  }
}

async function connect(ticket) {
  const lk = new Room({ adaptiveStream: true, dynacast: true });
  call.lk = lk;
  demo.call.lk = lk;
  lk.on(RoomEvent.ParticipantConnected, (p) => {
    log('call_peer_joined', { who: p.identity });
    if (call?.kind === 'direct') setState('in-call', `in call with ${call.peer}`);
    renderTiles();
  });
  lk.on(RoomEvent.ParticipantDisconnected, (p) => {
    log('call_peer_left', { who: p.identity });
    renderTiles();
    if (call?.kind === 'direct' && call.state === 'in-call') endCall(`${call.peer} left the call`);
  });
  lk.on(RoomEvent.TrackSubscribed, () => renderTiles());
  lk.on(RoomEvent.TrackUnsubscribed, () => renderTiles());
  lk.on(RoomEvent.LocalTrackPublished, () => renderTiles());
  lk.on(RoomEvent.Disconnected, () => { if (call && call.lk === lk) endCall('disconnected'); });
  await lk.connect(onThisHost(ticket.url), ticket.token);
  try {
    await lk.localParticipant.enableCameraAndMicrophone();
  } catch (e) {
    toast(`camera or microphone unavailable: ${e.message}`);
  }
  if (lk.remoteParticipants.size > 0 && call.kind === 'direct') setState('in-call', `in call with ${call.peer}`);
  renderTiles();
}

function participantName(identity) {
  // Chat names a participant <user>/<device> (the ticket's subject).
  return identity.split('/')[0];
}

function renderTiles() {
  const tiles = $('tiles');
  tiles.replaceChildren();
  const lk = call?.lk;
  if (!lk) return;
  const people = [lk.localParticipant, ...lk.remoteParticipants.values()];
  for (const p of people) {
    const tile = el('div', { class: 'tile', dataset: { participant: p.identity } });
    const cam = p.getTrackPublication(Track.Source.Camera)?.track;
    if (cam) {
      const v = cam.attach();
      v.muted = true;
      tile.append(v);
    } else {
      tile.append(el('video', {}));
    }
    if (p !== lk.localParticipant) {
      const mic = p.getTrackPublication(Track.Source.Microphone)?.track;
      if (mic) tile.append(mic.attach());
    }
    tile.append(el('span', { class: 'name' }, p === lk.localParticipant ? `${session.user} (you)` : participantName(p.identity)));
    tiles.append(tile);
  }
  demo.call.remotes = lk.remoteParticipants.size;
  demo.call.remoteVideo = [...lk.remoteParticipants.values()].filter((p) => p.getTrackPublication(Track.Source.Camera)?.track).length;
}

function onCallEvent(type, m) {
  log(type, { call: m.call, by: m.by });
  if (ringing?.callId === m.call && type !== 'call_ringing') {
    if (!(type === 'call_answered' && m.by === session.user && answeringHere)) {
      stopRing(type === 'call_answered' ? 'answered on another device' : type.replace('call_', 'call '));
    }
  }
  if (call?.callId !== m.call) return;
  if (call.kind === 'group') {
    // A group call (feat/group-calls): it goes on as people come and go; a move to a new
    // generation (someone put out) takes everyone else to it with a fresh ticket.
    if (type === 'call_moved') {
      if (m.expelled === session.user) endCall(`${m.by} put you out of the call`);
      else rejoinGroup(`${m.expelled ?? 'someone'} was put out; reconnecting`);
    }
    if (type === 'call_left' && m.by !== session.user) toast(`${m.by} left the call`);
    if (type === 'call_ended') endCall(m.by && m.by !== session.user ? `${m.by} ended the call` : 'the call ended');
    return;
  }
  if (type === 'call_answered' && call.role === 'caller') setState(call.lk?.remoteParticipants.size ? 'in-call' : 'connecting', `${m.by} answered`);
  if (type === 'call_declined' && m.by !== session.user) endCall(`${m.by} declined`);
  if (type === 'call_missed') endCall('no answer');
  if (type === 'call_cancelled' && m.by !== session.user) endCall('cancelled');
  if (type === 'call_ended' && m.by !== session.user) endCall(`${m.by} ended the call`);
}

let answeringHere = false;
let ringTimer = null;
let tone = null;

function showRing(r) {
  if (ringing?.callId === r.callId) return;
  ringing = r;
  demo.call.ringing = { from: r.from, mode: r.mode };
  log('ringing', { from: r.from, mode: r.mode });
  $('ring-title').textContent = `${r.from} is calling`;
  $('ring-detail').textContent = r.mode === 'server' ? 'rung by chat' : 'rung by the page (this build\'s chat does not ring)';
  if (!$('ring').open) $('ring').showModal();
  startTone();
  clearTimeout(ringTimer);
  ringTimer = setTimeout(() => stopRing('missed'), Math.max(1000, r.expires - Date.now()));
}

function stopRing(why) {
  if (!ringing) return;
  ringing = null;
  demo.call.ringing = null;
  clearTimeout(ringTimer);
  stopTone();
  if ($('ring').open) $('ring').close();
  if (why) toast(why);
}

async function answer() {
  const r = ringing;
  if (!r) return;
  answeringHere = true;
  stopRing();
  if (call) await hangUp();
  const group = roomById(r.room)?.kind === 'group';
  call = { room: r.room, callId: r.callId, peer: r.from, role: 'callee', kind: group ? 'group' : 'direct', mode: r.mode, state: 'connecting' };
  showPanel(group ? `${roomById(r.room).name}: group call` : `Call with ${r.from}`);
  try {
    const ticket = await ticketFor(r.room);
    if (r.mode === 'server' && !ticket.call) { endCall('the call ended while answering'); return; }
    if (r.mode === 'app') sendBody(r.room, { t: 'ring', what: 'answer', call: r.callId });
    await connect(ticket);
    log('call_answered_here', { from: r.from });
    setState(group || call.lk.remoteParticipants.size ? 'in-call' : 'connecting', group ? 'in the group call' : `in call with ${r.from}`);
  } catch (e) {
    endCall(`could not answer: ${e.message}`);
  } finally {
    answeringHere = false;
  }
}

function decline() {
  const r = ringing;
  if (!r) return;
  stopRing();
  if (r.mode === 'server') chat.send({ type: 'call_decline', room: r.room, call: r.callId });
  else sendBody(r.room, { t: 'ring', what: 'decline', call: r.callId });
  log('declined', { from: r.from });
}

async function hangUp(reason = 'call ended') {
  const c = call;
  if (!c) return;
  if (c.kind === 'group' && c.callId) {
    // Group calls (feat/group-calls): leave the call; it goes on for the others, and the last
    // one out ends it.
    chat.send({ type: 'call_leave', room: c.room, call: c.callId });
  } else if (c.kind === 'direct' && c.callId) {
    const answered = c.state === 'in-call' || c.role === 'callee' || c.answered;
    if (c.mode === 'server') {
      chat.send({ type: c.role === 'caller' && !answered ? 'call_cancel' : 'call_end', room: c.room, call: c.callId });
    } else {
      sendBody(c.room, { t: 'ring', what: c.role === 'caller' && !answered ? 'cancel' : 'end', call: c.callId });
    }
  }
  endCall(reason);
}

function endCall(reason) {
  const c = call;
  if (!c) return;
  call = null;
  c.state = 'ended';
  c.lk?.disconnect();
  demo.call.state = 'idle';
  demo.call.remotes = 0;
  log('call_ended_here', { reason });
  hidePanel();
  toast(reason);
}

function setState(state, detail) {
  if (!call) return;
  call.state = state;
  if (state === 'in-call') call.answered = true;
  demo.call.state = state;
  $('call-status').textContent = detail;
}

function showPanel(title) {
  $('call-panel').hidden = false;
  $('end-for-all').hidden = !(call?.kind === 'group');
  $('call-title').textContent = title;
  $('call-status').textContent = '';
  $('toggle-mic').textContent = 'Mute';
  $('toggle-cam').textContent = 'Camera off';
  document.querySelector('[data-tab="calls"]').click();
}

function hidePanel() {
  $('call-panel').hidden = true;
  $('tiles').replaceChildren();
}

// A two-tone ring from WebAudio, until answered, declined or timed out.
function startTone() {
  stopTone();
  try {
    const ctx = new AudioContext();
    const gain = ctx.createGain();
    gain.gain.value = 0;
    gain.connect(ctx.destination);
    for (const f of [440, 480]) {
      const o = ctx.createOscillator();
      o.frequency.value = f;
      o.connect(gain);
      o.start();
    }
    let on = false;
    const timer = setInterval(() => { on = !on; gain.gain.setTargetAtTime(on ? 0.08 : 0, ctx.currentTime, 0.02); }, 1000);
    tone = { ctx, timer };
  } catch { /* no audio: the dialog still shows */ }
}

function stopTone() {
  if (!tone) return;
  clearInterval(tone.timer);
  tone.ctx.close();
  tone = null;
}

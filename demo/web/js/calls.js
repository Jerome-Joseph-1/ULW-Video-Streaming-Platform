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
let call = null; // { room, callId, peer, role, mode, lk, state, kind, mic }
let ringing = null; // { room, callId, from, mode, expires, heard }
// Whether chat takes `answer` on a ticket (calls.md): a chat without it refuses the field, and
// the page then asks as before.
let answerField = true;
// Chat announces a ringing call again every 15 s: a ring not heard of for longer than this has
// ended without the page hearing how (its socket was away when it did).
const RING_SILENCE_MS = 20_000;
// Rings heard from others, by call id: who rang. A ticket that names one of these answered it.
const rungBy = new Map();
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
  $('toggle-mic').addEventListener('click', () => toggleMic());
  $('resume-audio').addEventListener('click', () => {
    call?.lk?.startAudio().catch(() => {}).finally(renderAudio);
  });
  // Escape would close the ring with nothing said to chat, and nothing would show it again.
  $('ring').addEventListener('cancel', (e) => e.preventDefault());
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
    heardRing(m.call, m.from);
    if (call && call.callId === m.call) return;
    if (call && call.room === m.room && call.state !== 'ended') {
      // Already in this room's call, and the other member rang it again (they came back after
      // chat forgot the call, or rang while this page rang them): answer by asking for a
      // ticket, and keep the connection (calls.md, Ringing).
      if (call.kind === 'direct' && call.mode === 'server') answerInCall(call, m);
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

function heardRing(callId, from) {
  rungBy.delete(callId);
  rungBy.set(callId, from);
  if (rungBy.size > 32) rungBy.delete(rungBy.keys().next().value);
}

// Asks chat for a ticket to the room's call: the answer is a `ticket` or an `error`. With
// `answering`, the ticket answers that ring, and a ring that is over is answered no_call naming
// it. A no_call that names no call answers a decline, cancel or end, never this.
async function ticketFor(room, { answering = null } = {}) {
  const ask = { type: 'call', room, device: device() };
  if (answering && answerField) ask.answer = answering;
  chat.send(ask);
  const m = await chat.next((x) =>
    (x.type === 'ticket' && x.room === room) ||
    (x.type === 'error' && x.room === room && (x.reason !== 'no_call' || (ask.answer && x.call === ask.answer))) ||
    // A chat that does not know `answer` refuses the ask as malformed, with no room.
    (ask.answer && x.type === 'error' && !x.room && x.reason === 'malformed'), 15_000);
  if (!m) throw new Error('no answer from chat');
  if (m.type === 'error' && !m.room) {
    answerField = false;
    demo.call.answerField = false;
    return ticketFor(room);
  }
  if (m.type === 'error') throw Object.assign(new Error(m.reason), { reason: m.reason, retry: m.retry_after_ms });
  return m;
}
demo.call.answerField = answerField;

async function startCall(peer) {
  if (call) { toast('already in a call'); return; }
  const room = directRoomWith(peer).id;
  call = { room, peer, role: 'caller', kind: 'direct', state: 'calling' };
  showPanel(`Calling ${peer}...`);
  setState('calling', `calling ${peer}`);
  let ticket;
  const mine = call;
  try {
    ticket = await ticketFor(room);
  } catch (e) {
    if (call !== mine) return;
    endCall(e.reason === 'ring_limited' ? `cannot ring ${peer} again yet (${Math.ceil((e.retry ?? 0) / 1000)} s)` : `call refused: ${e.message}`);
    return;
  }
  if (call !== mine) {
    // Hung up while chat issued the ticket: the ring it started is given up at once.
    if (ticket.call) chat.send({ type: 'call_cancel', room, call: ticket.call });
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
  try {
    await connect(ticket);
  } catch (e) {
    // Hung up while connecting is no failure; anything else ends the call (and its ring).
    if (call === mine) hangUp(`could not connect: ${e.message}`);
    return;
  }
  // An app-mode call nobody answers stops after the ring's 45 s, as chat's does.
  if (call?.mode === 'app') setTimeout(() => { if (call === mine && call.state === 'calling') hangUp('no answer'); }, 46_000);
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
  const c = call;
  const lk = new Room({ adaptiveStream: true, dynacast: true });
  c.lk = lk;
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
  // The microphone as it really is, whatever ends, mutes or replaces it.
  for (const ev of [RoomEvent.LocalTrackPublished, RoomEvent.LocalTrackUnpublished, RoomEvent.TrackMuted, RoomEvent.TrackUnmuted]) {
    lk.on(ev, () => { if (call === c) checkMic(c); });
  }
  lk.on(RoomEvent.MediaDevicesChanged, () => { if (call === c) devicesChanged(c); });
  lk.on(RoomEvent.AudioPlaybackStatusChanged, () => renderAudio());
  await lk.connect(onThisHost(ticket.url), ticket.token);
  if (call !== c || c.lk !== lk) { lk.disconnect(); return; }
  try {
    await lk.localParticipant.enableCameraAndMicrophone();
  } catch (e) {
    toast(`camera or microphone unavailable: ${e.message}`);
  }
  if (call !== c) return;
  // A new connection of the same call (a group call moved on) keeps the user's mute.
  if (c.mic && !c.mic.wanted) await lk.localParticipant.setMicrophoneEnabled(false).catch(() => {});
  watchMic(c);
  if (lk.remoteParticipants.size > 0 && c.kind === 'direct') setState('in-call', `in call with ${c.peer}`);
  renderTiles();
  renderAudio();
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
  if (ringing?.callId === r.callId) {
    // Announced again: it still rings.
    ringing.heard = Date.now();
    return;
  }
  ringing = { ...r, heard: Date.now() };
  demo.call.ringing = { from: r.from, mode: r.mode, call: r.callId };
  log('ringing', { from: r.from, mode: r.mode });
  $('ring-title').textContent = `${r.from} is calling`;
  $('ring-detail').textContent = r.mode === 'server' ? 'rung by chat' : 'rung by the page (this build\'s chat does not ring)';
  if (!$('ring').open) $('ring').showModal();
  startTone();
  clearTimeout(ringTimer);
  // Until the ring's own end, or until chat stops announcing it: a ring whose end this page
  // never heard (its socket was away) would otherwise stay up, and answering it would ring
  // nobody.
  const check = () => {
    const now = Date.now();
    if (!ringing) return;
    if (now >= ringing.expires) stopRing('missed');
    else if (ringing.mode === 'server' && now - ringing.heard > RING_SILENCE_MS) stopRing(`${ringing.from}'s call ended`);
    else ringTimer = setTimeout(check, 1000);
  };
  ringTimer = setTimeout(check, Math.min(1000, Math.max(0, r.expires - Date.now())));
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
  const mine = call;
  showPanel(group ? `${roomById(r.room).name}: group call` : `Call with ${r.from}`);
  setState('connecting', `answering ${r.from}`);
  const over = `${r.from}'s call ended before you answered`;
  try {
    const ticket = await ticketFor(r.room, { answering: r.mode === 'server' ? r.callId : null });
    if (call !== mine) return;
    if (r.mode === 'server') {
      if (!ticket.call) { endCall(over); return; }
      if (ticket.call !== r.callId && !group && rungBy.get(ticket.call) !== r.from) {
        // The ring was over and this chat, which does not take `answer`, started a new call
        // from here instead: give that up at once rather than ring the caller back.
        chat.send({ type: 'call_cancel', room: r.room, call: ticket.call });
        endCall(over);
        return;
      }
      // The caller rang again meanwhile: the ticket answered that call, which is this one now.
      call.callId = ticket.call;
      demo.call.callId = ticket.call;
    }
    if (r.mode === 'app') sendBody(r.room, { t: 'ring', what: 'answer', call: r.callId });
    await connect(ticket);
    if (call !== mine) return;
    log('call_answered_here', { from: r.from });
    setState(group || call.lk.remoteParticipants.size ? 'in-call' : 'connecting', group ? 'in the group call' : `in call with ${r.from}`);
  } catch (e) {
    if (call !== mine) return;
    endCall(e.reason === 'no_call' ? over : `could not answer: ${e.message}`);
  } finally {
    answeringHere = false;
  }
}

// In this room's call already, and rung for it again: a ticket answers that ring, and its call
// is this one's from now on. The connection stays as it is.
async function answerInCall(c, m) {
  try {
    const ticket = await ticketFor(m.room, { answering: m.call });
    if (call === c && ticket.call) {
      c.callId = ticket.call;
      demo.call.callId = ticket.call;
    }
  } catch { /* the ring runs out by itself; the call goes on in LiveKit */ }
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
  unwatchMic(c);
  c.lk?.disconnect();
  // Nothing of this call is left for the next: its id, its connection, its microphone.
  Object.assign(demo.call, { state: 'idle', remotes: 0, remoteVideo: 0, callId: null, lk: null, mic: null });
  log('call_ended_here', { reason });
  hidePanel();
  toast(reason);
}

function setState(state, detail) {
  if (!call) return;
  call.state = state;
  if (state === 'in-call') call.answered = true;
  demo.call.state = state;
  demo.call.callId = call.callId ?? null;
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
  $('mic-note').hidden = true;
  $('resume-audio').hidden = true;
}

// ---- The microphone
//
// The system may take the microphone mid-call (a phone call on the device, another app): the
// track then ends, or stays muted by the system, and LiveKit stops sending it (after 5 s of
// mute) or mutes it when it cannot get it back. Nothing gives it back by itself: the page checks
// it every second and on every sign of a change (the page shown again, a device plugged in or
// out), and gets a new one from getUserMedia, put in place of the old one on the same
// publication, as long as the user wants the microphone on. The button shows what the user
// chose; a note says when the microphone is not there.

// How long a track may stay ended or muted by the system before the page replaces it: LiveKit
// tries once itself when a track ends, and a short mute (a device switching) comes back alone.
const MIC_ENDED_GRACE_MS = 1500;
const MIC_MUTED_GRACE_MS = 3000;
const MIC_RETRY_MAX_MS = 10_000;

function micTrack(c) {
  return c?.lk?.localParticipant.getTrackPublication(Track.Source.Microphone)?.track ?? null;
}

function watchMic(c) {
  if (c.mic) return;
  c.mic = { wanted: true, brokenSince: null, retryAt: 0, failures: 0, interrupted: false, restarts: 0, busy: false };
  const wake = () => {
    if (document.visibilityState === 'hidden' || call !== c) return;
    c.mic.retryAt = 0;
    checkMic(c);
    c.lk?.startAudio().catch(() => {}).finally(renderAudio);
  };
  c.micWake = wake;
  document.addEventListener('visibilitychange', wake);
  window.addEventListener('focus', wake);
  window.addEventListener('pageshow', wake);
  c.micTimer = setInterval(() => checkMic(c), 1000);
  checkMic(c);
}

function unwatchMic(c) {
  clearInterval(c.micTimer);
  if (c.micWake) {
    document.removeEventListener('visibilitychange', c.micWake);
    window.removeEventListener('focus', c.micWake);
    window.removeEventListener('pageshow', c.micWake);
  }
}

// Whether the track no longer carries the microphone, and since when.
function micBroken(c, t, now) {
  const mst = t.mediaStreamTrack;
  const ended = !mst || mst.readyState !== 'live';
  const broken = ended || mst.muted || t.isUpstreamPaused;
  if (!broken) {
    c.mic.brokenSince = null;
    return false;
  }
  c.mic.brokenSince ??= now;
  return now - c.mic.brokenSince >= (ended ? MIC_ENDED_GRACE_MS : MIC_MUTED_GRACE_MS) || t.isUpstreamPaused;
}

async function checkMic(c) {
  if (call !== c || !c.mic || c.mic.busy) { renderMic(); return; }
  const t = micTrack(c);
  const now = Date.now();
  if (!t) {
    // Not published (no microphone when the call started): published once there is one.
    if (c.mic.wanted && now >= c.mic.retryAt && c.lk) await recoverMic(c, null, false);
    renderMic();
    return;
  }
  const broken = micBroken(c, t, now);
  if (!c.mic.wanted || (!broken && !t.isMuted)) {
    if (!broken && !t.isMuted) Object.assign(c.mic, { interrupted: false, failures: 0 });
    renderMic();
    return;
  }
  if (now < c.mic.retryAt) { renderMic(); return; }
  await recoverMic(c, t, broken);
}

async function recoverMic(c, t, broken) {
  c.mic.busy = true;
  try {
    if (!t) {
      await c.lk.localParticipant.setMicrophoneEnabled(true);
    } else {
      if (broken) {
        await t.restartTrack();
        c.mic.restarts += 1;
      }
      if (t.isMuted) await t.unmute();
      if (t.isUpstreamPaused) await t.resumeUpstream();
    }
    Object.assign(c.mic, { interrupted: false, failures: 0, brokenSince: null, retryAt: 0 });
    log('mic_recovered', { restarted: broken });
  } catch (e) {
    // Still taken (a phone call goes on): tried again, less and less often, and at once when
    // the page is shown again or the user taps the button.
    c.mic.failures += 1;
    c.mic.interrupted = true;
    c.mic.retryAt = Date.now() + Math.min(1000 * 2 ** (c.mic.failures - 1), MIC_RETRY_MAX_MS);
    log('mic_unavailable', { error: e?.name ?? String(e) });
  } finally {
    c.mic.busy = false;
    renderMic();
  }
}

async function toggleMic() {
  const c = call;
  if (!c?.lk) return;
  if (!c.mic) watchMic(c);
  c.mic.wanted = !c.mic.wanted;
  renderMic();
  try {
    if (c.mic.wanted) {
      Object.assign(c.mic, { retryAt: 0, failures: 0 });
      const t = micTrack(c);
      // A track the system ended or keeps muted is replaced, not just unmuted: unmuting it
      // would send nothing.
      const mst = t?.mediaStreamTrack;
      await recoverMic(c, t, !!t && (!mst || mst.readyState !== 'live' || mst.muted || t.isUpstreamPaused));
    } else {
      await c.lk.localParticipant.setMicrophoneEnabled(false);
    }
  } catch (e) {
    toast(`microphone: ${e.message}`);
  }
  renderMic();
}

// A microphone unplugged or switched: back to the system's default, then checked.
async function devicesChanged(c) {
  try {
    const active = c.lk.getActiveDevice('audioinput');
    const inputs = await Room.getLocalDevices('audioinput', false);
    if (active && active !== 'default' && !inputs.some((d) => d.deviceId === active)) {
      await c.lk.switchActiveDevice('audioinput', 'default');
    }
  } catch { /* checked below all the same */ }
  if (c.mic) c.mic.retryAt = 0;
  checkMic(c);
}

function renderMic() {
  const c = call;
  const t = micTrack(c);
  const mst = t?.mediaStreamTrack;
  if (!c?.mic) {
    demo.call.mic = null;
    return;
  }
  demo.call.mic = {
    wanted: c.mic.wanted,
    enabled: !!c.lk?.localParticipant.isMicrophoneEnabled,
    live: !!mst && mst.readyState === 'live' && !mst.muted && !t.isMuted && !t.isUpstreamPaused,
    track: mst?.id ?? null,
    interrupted: c.mic.interrupted,
    restarts: c.mic.restarts,
  };
  $('toggle-mic').textContent = c.mic.wanted ? 'Mute' : 'Unmute';
  $('mic-note').hidden = !(c.mic.wanted && c.mic.interrupted);
}

// The other side's audio, which a system interruption (or a browser's autoplay rule) may have
// stopped: a tap starts it again.
function renderAudio() {
  const lk = call?.lk;
  $('resume-audio').hidden = !lk || lk.canPlaybackAudio;
  demo.call.audioBlocked = !!lk && !lk.canPlaybackAudio;
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

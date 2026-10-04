// Live: go live from the camera over WHIP through the gateway's stream service, watch others'
// streams as HLS, and get the recording as a video when the stream ends
// (docs/integration/live.md; the stream service is feat/live-publish, #141).
//
// There is no endpoint that lists streams, so the page announces its own in the team room
// ({t:'live'} messages) and checks each announced id with GET /api/v1/live/{id}.
import { $, api, apiUrl, chat, decodeBody, demo, el, log, myRooms, onThisHost, sendBody, session, toast, utf8, b64url } from './core.js';
import { onAppMessage } from './chat.js';
import { addVideo, attachHls, refreshPlaybackToken } from './videos.js';

let broadcast = null; // { id, pc, media, resource, poll }
let viewer = null; // { id, hls, room, timer }
const streams = new Map(); // id -> { id, owner, state }
demo.live = { streams, state: 'idle' };

const announceRoom = () => myRooms().find((r) => r.kind === 'group' && !r.e2ee);

export function initLive() {
  $('go-live').addEventListener('click', () => goLive().catch(async (e) => {
    liveStatus(`could not go live: ${e.message}`);
    await abandon();
  }));
  $('end-live').addEventListener('click', () => endLive());
  $('watch-form').addEventListener('submit', (e) => {
    e.preventDefault();
    const id = $('watch-id').value.trim();
    if (id) watch(id);
  });
  $('live-composer').addEventListener('submit', (e) => {
    e.preventDefault();
    const text = $('live-message').value.trim();
    if (!text || !viewer?.room) return;
    chat.send({ type: 'send', room: viewer.room, id: crypto.randomUUID(), body: b64url.encode(utf8.encode(text)) });
    $('live-message').value = '';
  });
  onAppMessage(({ sender, body }) => {
    if (body.t !== 'live' || typeof body.id !== 'string') return;
    const s = streams.get(body.id) ?? { id: body.id, owner: sender, state: 'unknown' };
    if (body.what === 'ended') s.state = 'ended';
    streams.set(body.id, s);
    refresh(body.id);
  });
  // A stream's live chat (lossy): the room chat.js does not own.
  chat.addEventListener('joined', (e) => {
    if (viewer && viewer.joining && e.m.room) { viewer.room = e.m.room; viewer.joining = false; }
  });
  chat.addEventListener('message', (e) => {
    if (!viewer || e.m.room !== viewer.room) return;
    const body = decodeBody(e.m.body);
    $('live-messages').append(el('li', { class: e.m.sender === session.user ? 'mine' : '' },
      el('div', { class: 'who' }, e.m.sender), el('div', {}, body.text ?? '')));
    $('live-messages').scrollTop = $('live-messages').scrollHeight;
  });
  chat.addEventListener('error', (e) => {
    if (viewer?.joining && (e.m.reason === 'not_live' || e.m.reason === 'bad_stream')) {
      viewer.joining = false;
      $('live-composer').hidden = true;
      $('live-messages').replaceChildren(el('li', { class: 'system' }, `live chat unavailable (${e.m.reason})`));
    }
  });
  setInterval(() => { for (const id of streams.keys()) refresh(id); }, 5000);
}

async function refresh(id) {
  const r = await api('GET', `/api/v1/live/${id}`).catch(() => null);
  const s = streams.get(id);
  if (!s) return;
  if (r?.ok && r.data) s.state = r.data.state;
  else if (r?.status === 404) s.state = 'gone';
  renderStreams();
}

function renderStreams() {
  const list = $('streams');
  list.replaceChildren();
  const shown = [...streams.values()].filter((s) => s.state !== 'gone').reverse();
  if (shown.length === 0) list.append(el('li', { class: 'muted' }, 'No streams yet. When someone goes live it shows here.'));
  for (const s of shown) {
    list.append(el('li', { dataset: { stream: s.id, state: s.state } },
      el('span', { class: 'grow', title: s.id }, `${s.owner}'s stream`),
      el('span', { class: `state ${s.state}` }, s.state),
      el('button', { onclick: () => watch(s.id), disabled: s.state === 'live' || s.state === 'starting' ? null : '' }, 'Watch')));
  }
}

const liveStatus = (text) => { $('live-status').textContent = text; demo.live.status = text; };

async function goLive() {
  if (broadcast) return;
  $('go-live').disabled = true;
  liveStatus('starting a stream...');
  const created = await api('POST', '/api/v1/live');
  if (created.status === 404 || created.status === 405) {
    $('go-live').disabled = false;
    liveStatus('This build\'s gateway has no stream service yet: going live arrives with feat/live-publish (#141). ' +
      'Run up.sh again once it is on main (or with DEMO_IMAGE_TAG naming a build that has it).');
    return;
  }
  if (created.status !== 201 && created.status !== 200) {
    $('go-live').disabled = false;
    liveStatus(`the stream service refused: ${created.status}${created.status === 503 ? ' (busy: the demo runs one stream at a time; end the other one first)' : ''}`);
    return;
  }
  const stream = created.data;
  broadcast = { id: stream.id };
  demo.live.id = stream.id;
  log('live_created', { id: stream.id });
  const fresh = async () => {
    const t = await api('POST', `/api/v1/live/${stream.id}/ticket`);
    if (!t.ok) throw new Error(`ticket answered ${t.status}`);
    return `Bearer ${t.data.token}`;
  };

  const media = await navigator.mediaDevices.getUserMedia({ audio: true, video: { width: 1280, height: 720, frameRate: 30 } });
  broadcast.media = media;
  $('live-preview').srcObject = media;
  const pc = new RTCPeerConnection({ bundlePolicy: 'max-bundle' });
  broadcast.pc = pc;
  for (const track of media.getTracks()) pc.addTransceiver(track, { direction: 'sendonly' });
  const offer = await pc.createOffer();
  // WHIP (RFC 9725) as live.md describes: the offer first, then the ICE servers from the
  // answer's Link headers, then gathering, then the candidates in a PATCH with a fresh ticket.
  const whip = onThisHost(stream.publish.url);
  liveStatus('publishing over WHIP...');
  const posted = await fetch(whip, {
    method: 'POST',
    headers: { 'content-type': 'application/sdp', authorization: `Bearer ${stream.publish.token}` },
    body: offer.sdp,
  });
  if (posted.status !== 201) throw new Error(`WHIP POST answered ${posted.status}`);
  const iceServers = (posted.headers.get('link') ?? '').split(',')
    .map((link) => /^\s*<([^>]+)>\s*;\s*rel="?ice-server"?(.*)$/.exec(link))
    .filter((link) => link !== null)
    .map(([, urls, params]) => ({ urls, username: /username="([^"]*)"/.exec(params)?.[1], credential: /credential="([^"]*)"/.exec(params)?.[1] }));
  pc.setConfiguration({ bundlePolicy: 'max-bundle', iceServers });
  const candidates = [];
  const gathered = new Promise((resolve) => {
    pc.addEventListener('icecandidate', (e) => {
      if (e.candidate?.candidate) candidates.push(e.candidate);
      if (!e.candidate) resolve();
    });
    setTimeout(resolve, 3000);
  });
  await pc.setLocalDescription(offer);
  await pc.setRemoteDescription({ type: 'answer', sdp: await posted.text() });
  await gathered;
  const first = pc.getTransceivers()[0];
  const attribute = (name) => offer.sdp.split('\r\n').find((l) => l.startsWith(`a=${name}:`));
  const fragment = [attribute('ice-ufrag'), attribute('ice-pwd'), `m=${first.sender.track.kind} 9 UDP/TLS/RTP/SAVPF 0`, `a=mid:${first.mid}`,
    ...candidates.filter((c) => c.sdpMid === first.mid).map((c) => `a=${c.candidate}`), ''];
  broadcast.resource = new URL(posted.headers.get('location'), whip).href;
  await fetch(broadcast.resource, {
    method: 'PATCH',
    headers: { 'content-type': 'application/trickle-ice-sdpfrag', 'if-match': posted.headers.get('etag'), authorization: await fresh() },
    body: fragment.join('\r\n'),
  });
  await new Promise((resolve, reject) => {
    const check = () => {
      if (pc.connectionState === 'connected') resolve();
      if (pc.connectionState === 'failed') reject(new Error('media connection failed'));
    };
    pc.addEventListener('connectionstatechange', check);
    check();
    setTimeout(() => reject(new Error('media did not connect within 20 s')), 20_000);
  });
  liveStatus('media connected; starting the packager and the recorder...');
  // start is idempotent; retry it on 503 (live.md), within the recorder's 30 s.
  let live;
  for (let attempt = 0; attempt < 6; ++attempt) {
    live = await api('POST', `/api/v1/live/${stream.id}/start`);
    if (live.status !== 503) break;
    await new Promise((resolve) => setTimeout(resolve, 2000));
  }
  if (!live.ok) throw new Error(`start answered ${live.status}`);
  demo.live.state = 'live';
  log('live_started', { id: stream.id });
  liveStatus(`LIVE. Stream ${stream.id}. Viewers find it in their Live tab.`);
  $('end-live').disabled = false;
  const room = announceRoom();
  if (room) sendBody(room.id, { t: 'live', what: 'started', id: stream.id });
  streams.set(stream.id, { id: stream.id, owner: session.user, state: 'live' });
  renderStreams();
  broadcast.fresh = fresh;
}

// A stream that never went live: end it, so the demo's one stream slot is free again.
async function abandon() {
  const b = broadcast;
  broadcast = null;
  $('go-live').disabled = false;
  $('end-live').disabled = true;
  if (!b) return;
  await api('POST', `/api/v1/live/${b.id}/end`).catch(() => {});
  b.pc?.close();
  for (const t of b.media?.getTracks() ?? []) t.stop();
  $('live-preview').srcObject = null;
}

async function endLive() {
  const b = broadcast;
  if (!b) return;
  $('end-live').disabled = true;
  liveStatus('ending...');
  // The WHIP session first, with a fresh ticket while the stream still issues them; then the
  // stream itself (either ends it; end is idempotent and says so).
  if (b.resource && b.fresh) {
    const auth = await b.fresh().catch(() => null);
    if (auth) await fetch(b.resource, { method: 'DELETE', headers: { authorization: auth } }).catch(() => {});
  }
  const ended = await api('POST', `/api/v1/live/${b.id}/end`);
  b.pc?.close();
  for (const t of b.media?.getTracks() ?? []) t.stop();
  $('live-preview').srcObject = null;
  broadcast = null;
  demo.live.state = 'ended';
  log('live_ended', { id: b.id, status: ended.status });
  const room = announceRoom();
  if (room) sendBody(room.id, { t: 'live', what: 'ended', id: b.id });
  $('go-live').disabled = false;
  liveStatus('ended; the recording becomes a video (waiting for it)...');
  // The owner's GET names the recording's video once it is queued.
  for (let i = 0; i < 120; ++i) {
    const r = await api('GET', `/api/v1/live/${b.id}`);
    if (r.data?.video_id) {
      addVideo(r.data.video_id, `Live stream ${new Date().toLocaleTimeString()}`, 'processing');
      demo.live.videoId = r.data.video_id;
      log('live_recording', { video: r.data.video_id });
      liveStatus('the recording is in Upload & Watch, transcoding; it plays once ready.');
      toast('Your live recording is in Upload & Watch');
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 2000));
  }
  liveStatus('no recording appeared within 4 minutes (a stream with no media becomes no video)');
}

async function watch(id) {
  viewer?.hls?.destroy();
  clearInterval(viewer?.timer);
  await refreshPlaybackToken();
  $('viewer').hidden = false;
  $('viewer-title').textContent = `Watching ${streams.get(id)?.owner ?? id}'s stream`;
  $('viewer-status').textContent = 'waiting for the first segments...';
  $('live-messages').replaceChildren();
  $('live-composer').hidden = false;
  const video = $('live-player');
  const src = apiUrl(`/api/v1/live/${id}/index.m3u8`);
  viewer = { id, joining: true };
  // A stream just taken live has no playlist for a few seconds (404, live.md), and hls.js does
  // not retry a 404: wait for the first one here.
  for (let i = 0; i < 60; ++i) {
    const r = await api('GET', src).catch(() => null);
    if (r?.ok) break;
    if (viewer?.id !== id) return;
    $('viewer-status').textContent = `waiting for the stream's first segments (${r?.status ?? 'no answer'})...`;
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  await refreshPlaybackToken();
  const hls = attachHls(video, src, {
    liveSyncDurationCount: 2,
    liveMaxLatencyDurationCount: 5,
  });
  viewer.hls = hls;
  demo.live.viewer = { id, playing: false };
  hls.on(Hls.Events.MANIFEST_PARSED, () => video.play().catch(() => {}));
  hls.on(Hls.Events.ERROR, (_, data) => {
    log('live_hls_error', { details: data.details, fatal: data.fatal, status: data.response?.code });
    if (!data.fatal) return;
    $('viewer-status').textContent = `playback error: ${data.details}`;
    if (data.type === Hls.ErrorTypes.MEDIA_ERROR) hls.recoverMediaError();
  });
  video.addEventListener('playing', () => {
    demo.live.viewer.playing = true;
    log('live_playing', { id });
    $('viewer-status').textContent = 'playing';
  }, { once: true });
  video.addEventListener('ended', () => { $('viewer-status').textContent = 'the stream ended'; });
  viewer.timer = setInterval(() => {
    if (hls.latency) $('viewer-latency').textContent = `about ${hls.latency.toFixed(1)} s behind live`;
  }, 1000);
  chat.send({ type: 'join', stream: id, delivery: 'lossy' });
  document.querySelector('[data-tab="live"]').click();
}

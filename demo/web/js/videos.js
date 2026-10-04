// Upload & Watch: a resumable upload (docs/integration/uploads.md), the video's state polled
// until ready, and HLS playback through the gateway's playlists and the store's signed segment
// URLs (docs/integration/videos-and-playback.md).
import { $, api, apiOrigin, apiUrl, demo, el, log, store, toast, token } from './core.js';

let file = null;
let upload = null; // { uploadId, videoId, size, name, chunk, offset, paused }
let hls = null;
const polling = new Set();

export function initVideos() {
  $('file').addEventListener('change', () => {
    file = $('file').files[0] ?? null;
    $('upload').disabled = !file;
    if (file) status(`${file.name}, ${(file.size / 1048576).toFixed(1)} MiB`);
  });
  $('make-clip').addEventListener('click', async () => {
    $('make-clip').disabled = true;
    status('recording a test clip in this browser...');
    try {
      file = await makeClip();
      $('upload').disabled = false;
      status(`${file.name} recorded, ${(file.size / 1048576).toFixed(1)} MiB: press Upload`);
    } catch (e) {
      status(`could not record a clip: ${e.message}`);
    } finally {
      $('make-clip').disabled = false;
    }
  });
  $('upload').addEventListener('click', () => startUpload());
  $('pause').addEventListener('click', () => {
    if (!upload) return;
    if (upload.paused) {
      upload.paused = false;
      $('pause').textContent = 'Pause';
      resume();
    } else {
      upload.paused = true;
      upload.controller?.abort();
      $('pause').textContent = 'Resume';
      status(`paused at ${mib(upload.offset)} of ${mib(upload.size)} MiB; Resume asks the gateway where it stands (HEAD) and continues`);
    }
  });
  renderVideos();
  for (const v of store.get('videos', [])) {
    if (v.state !== 'ready' && v.state !== 'failed') poll(v.id);
  }
}

const mib = (n) => (n / 1048576).toFixed(1);
const status = (text) => { $('upload-status').textContent = text; };

// Remembers a video this user owns (an upload, or a live stream's recording).
export function addVideo(id, title, state = 'processing') {
  const videos = store.get('videos', []);
  if (!videos.some((v) => v.id === id)) {
    videos.unshift({ id, title, state, added: Date.now() });
    store.set('videos', videos);
  }
  renderVideos();
  poll(id);
}

function updateVideo(id, patch) {
  const videos = store.get('videos', []);
  const v = videos.find((x) => x.id === id);
  if (!v) return;
  Object.assign(v, patch);
  store.set('videos', videos);
  renderVideos();
}

function renderVideos() {
  const list = $('videos');
  list.replaceChildren();
  const videos = store.get('videos', []);
  demo.videos = videos;
  if (videos.length === 0) list.append(el('li', { class: 'muted' }, 'Nothing yet: upload a file or make a test clip.'));
  for (const v of videos) {
    const playButton = el('button', { onclick: () => play(v), disabled: v.state === 'ready' ? null : '' }, 'Play');
    list.append(el('li', { dataset: { video: v.id, state: v.state } },
      el('span', { class: 'grow', title: v.id }, v.title),
      el('span', { class: `state ${v.state}` }, v.state + (v.error ? `: ${v.error}` : '')),
      playButton));
  }
}

// GET /api/v1/videos/{id} every 2 s until ready or failed.
async function poll(id) {
  if (polling.has(id)) return;
  polling.add(id);
  try {
    for (;;) {
      const r = await api('GET', `/api/v1/videos/${id}`).catch(() => null);
      if (r?.ok && r.data) {
        updateVideo(id, { state: r.data.state, error: r.data.error_reason, duration: r.data.duration_ms,
          title: store.get('videos', []).find((v) => v.id === id)?.title ?? r.data.title });
        if (r.data.state === 'ready' || r.data.state === 'failed') {
          log('video_state', { id, state: r.data.state });
          if (r.data.state === 'ready') toast(`"${r.data.title}" is ready to play`);
          return;
        }
      }
      await new Promise((resolve) => setTimeout(resolve, 2000));
    }
  } finally {
    polling.delete(id);
  }
}

async function startUpload() {
  if (!file) return;
  // A file this window already started (same name, size and date) resumes where it stopped.
  const saved = store.get('upload', null);
  if (saved && saved.name === file.name && saved.size === file.size && saved.modified === file.lastModified) {
    upload = { ...saved, paused: false };
    status('resuming the earlier upload of this file');
  } else {
    const r = await api('POST', '/api/v1/uploads',
      { json: { filename: file.name, size_bytes: file.size, content_type: file.type || 'video/mp4' } });
    if (r.status !== 201) { status(`create refused: ${r.status}`); return; }
    upload = { uploadId: r.data.upload_id, videoId: r.data.video_id, size: file.size, name: file.name,
      modified: file.lastModified, chunk: r.data.chunk_size, offset: 0, paused: false };
    store.set('upload', { ...upload });
    addVideo(upload.videoId, file.name, 'uploading');
    log('upload_created', { video: upload.videoId });
  }
  $('upload').disabled = true;
  $('pause').disabled = false;
  $('pause').textContent = 'Pause';
  demo.upload = upload;
  await resume();
}

async function resume() {
  const u = upload;
  // Where the gateway says the upload stands; every reported offset is a safe restart point.
  const head = await api('HEAD', `/api/v1/uploads/${u.uploadId}`);
  if (head.status === 204) u.offset = Number(head.headers.get('upload-offset'));
  while (!u.paused && u.offset < u.size) {
    const end = Math.min(u.offset + u.chunk, u.size);
    progress(u.offset, u.size, 'uploading');
    u.controller = new AbortController();
    let r;
    try {
      r = await fetch(apiUrl(`/api/v1/uploads/${u.uploadId}`), {
        method: 'PATCH',
        signal: u.controller.signal,
        headers: { authorization: `Bearer ${await token()}`, 'upload-offset': String(u.offset),
          'content-type': 'application/offset+octet-stream' },
        body: file.slice(u.offset, end),
      });
    } catch (e) {
      if (u.paused) return;
      status(`network error (${e.message}); retrying from the server's offset`);
      await new Promise((resolve) => setTimeout(resolve, 1000));
      const again = await api('HEAD', `/api/v1/uploads/${u.uploadId}`);
      if (again.status === 204) u.offset = Number(again.headers.get('upload-offset'));
      continue;
    }
    if (r.status === 204 || r.status === 409) {
      u.offset = Number(r.headers.get('upload-offset'));
    } else if (r.status === 429 || r.status === 503) {
      await new Promise((resolve) => setTimeout(resolve, 1000 * Number(r.headers.get('retry-after') ?? 2)));
    } else {
      status(`upload refused: ${r.status}`);
      return;
    }
    store.set('upload', { ...u, controller: undefined });
  }
  if (u.paused) return;
  progress(u.size, u.size, 'committing');
  const c = await api('POST', `/api/v1/uploads/${u.uploadId}/commit`);
  if (!c.ok) { status(`commit refused: ${c.status}`); return; }
  store.set('upload', null);
  $('pause').disabled = true;
  $('upload').disabled = !file;
  status(`uploaded; the worker is transcoding it (${c.data.state})`);
  log('upload_committed', { video: u.videoId });
  updateVideo(u.videoId, { state: c.data.state });
  poll(u.videoId);
  upload = null;
}

function progress(done, total, what) {
  $('upload-progress').value = total ? done / total : 0;
  status(`${what}: ${mib(done)} of ${mib(total)} MiB`);
}

// hls.js with the token on the gateway's playlist requests only: segment URLs are signed for
// the store and take no credentials.
export function attachHls(video, src, options = {}) {
  const player = new Hls({
    ...options,
    xhrSetup: (xhr, url) => {
      if (new URL(url, location.href).origin === apiOrigin) {
        xhr.setRequestHeader('authorization', `Bearer ${demoToken()}`);
      }
    },
  });
  player.loadSource(src);
  player.attachMedia(video);
  return player;
}
let cachedToken = '';
const demoToken = () => cachedToken;
export async function refreshPlaybackToken() { cachedToken = await token(); }

async function play(v) {
  await refreshPlaybackToken();
  hls?.destroy();
  const video = $('player');
  $('player-title').textContent = v.title;
  $('player-status').textContent = 'loading...';
  hls = attachHls(video, apiUrl(`/api/v1/videos/${v.id}/master.m3u8`), { startLevel: -1 });
  hls.on(Hls.Events.MANIFEST_PARSED, (_, data) => {
    $('player-status').textContent = `${data.levels.length} renditions: ${data.levels.map((l) => `${l.height}p`).join(', ')}`;
    video.play().catch(() => {});
  });
  hls.on(Hls.Events.LEVEL_SWITCHED, (_, data) => {
    $('player-status').textContent = `playing ${hls.levels[data.level].height}p`;
  });
  hls.on(Hls.Events.ERROR, (_, data) => {
    if (data.fatal) $('player-status').textContent = `playback error: ${data.details}`;
  });
  video.addEventListener('playing', () => log('vod_playing', { id: v.id }), { once: true });
  demo.player = { hls, video, id: v.id };
}

// A short clip from an animated canvas and a tone, recorded with MediaRecorder: a demo needs
// no video file at hand.
async function makeClip(seconds = 6) {
  const canvas = el('canvas', { width: 640, height: 360 });
  const g = canvas.getContext('2d');
  const stream = canvas.captureStream(30);
  const audio = new AudioContext();
  const osc = audio.createOscillator();
  const dest = audio.createMediaStreamDestination();
  osc.frequency.value = 440;
  osc.connect(dest);
  osc.start();
  stream.addTrack(dest.stream.getAudioTracks()[0]);
  // MP4 first: Chrome's and Safari's MP4 recordings carry their duration, which a WebM one from
  // MediaRecorder lacks, and the worker refuses a source without one. Firefox records WebM only;
  // there, pick a file instead.
  const type = ['video/mp4', 'video/webm;codecs=vp8,opus', 'video/webm'].find((t) => MediaRecorder.isTypeSupported(t));
  const recorder = new MediaRecorder(stream, { mimeType: type, videoBitsPerSecond: 2_000_000 });
  const parts = [];
  recorder.ondataavailable = (e) => parts.push(e.data);
  const started = performance.now();
  let frame = 0;
  const draw = () => {
    const t = (performance.now() - started) / 1000;
    g.fillStyle = `hsl(${(t * 60) % 360} 60% 35%)`;
    g.fillRect(0, 0, 640, 360);
    g.fillStyle = '#fff';
    g.font = 'bold 42px system-ui, sans-serif';
    g.fillText('ULW demo clip', 40, 90);
    g.font = '28px system-ui, sans-serif';
    g.fillText(`${new Date().toLocaleTimeString()}  frame ${frame++}`, 40, 150);
    g.beginPath();
    g.arc(320 + 220 * Math.sin(t * 2), 260, 40, 0, 2 * Math.PI);
    g.fill();
  };
  const timer = setInterval(draw, 1000 / 30);
  draw();
  recorder.start(500);
  await new Promise((resolve) => setTimeout(resolve, seconds * 1000));
  await new Promise((resolve) => { recorder.onstop = resolve; recorder.stop(); });
  clearInterval(timer);
  osc.stop();
  audio.close();
  const mime = type.split(';')[0];
  const ext = mime === 'video/mp4' ? 'mp4' : 'webm';
  return new File(parts, `clip-${new Date().toISOString().slice(11, 19).replace(/:/g, '')}.${ext}`,
    { type: mime, lastModified: Date.now() });
}

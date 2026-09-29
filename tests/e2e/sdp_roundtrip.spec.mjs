// M24 acceptance: a Chromium offer goes through codec/sdp (parse, validate, serialize) and comes
// out byte for byte as it went in, CRLF line endings included; Chromium then accepts what the
// serializer wrote. Two peer connections in one page: the offerer's offer, round-tripped, is
// the answerer's remote description; the answer, round-tripped, is the offerer's; and the two
// reach ICE "connected" on the descriptions our serializer wrote.
//
// Needs only a build tree, no Postgres or MinIO:
//   tests/e2e/run.sh build/ci sdp_roundtrip.spec.mjs
// With ULW_SDP_CAPTURE=1 the run also rewrites tests/data/sdp from its offer and answer.
import { expect, test } from '@playwright/test';
import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const fixtures = path.resolve(here, '../data/sdp');
const tool = path.join(process.env.ULW_BUILD_DIR ?? path.resolve(here, '../../build/ci'),
  'tests', 'ulw_sdp_roundtrip');

// Parse, validate and serialize; throws with the tool's "line N: ..." if parsing fails.
function roundTrip(sdp) {
  return execFileSync(tool, { input: sdp }).toString();
}

function withCrlf(sdp) {
  return sdp.replace(/\r?\n/g, '\r\n');
}

// Audio from an oscillator and video from a canvas, so no capture device or flag is needed;
// video offered as three simulcast layers, and a data channel: every kind of m= section a
// call uses, and the rid and simulcast attributes.
async function offerer() {
  const pc = new RTCPeerConnection();
  window.offerer = pc;
  const audio = new AudioContext();
  const tone = audio.createOscillator();
  const sink = audio.createMediaStreamDestination();
  tone.connect(sink);
  tone.start();
  const canvas = document.createElement('canvas');
  canvas.width = 320;
  canvas.height = 240;
  const g = canvas.getContext('2d');
  setInterval(() => {
    g.fillStyle = `hsl(${Date.now() % 360}, 80%, 50%)`;
    g.fillRect(0, 0, canvas.width, canvas.height);
  }, 50);
  const video = canvas.captureStream(15).getVideoTracks()[0];
  const stream = new MediaStream([sink.stream.getAudioTracks()[0], video]);
  pc.addTrack(stream.getAudioTracks()[0], stream);
  pc.addTransceiver(video, {
    streams: [stream],
    sendEncodings: [
      { rid: 'q', scaleResolutionDownBy: 4 },
      { rid: 'h', scaleResolutionDownBy: 2 },
      { rid: 'f' },
    ],
  });
  pc.createDataChannel('chat');
  // Chromium's gathering on this host never reports "complete" (a network it cannot use keeps
  // it waiting), so a description is taken once it holds a candidate: the fixture then carries
  // a=candidate lines, and each side has one to try. Functions run in the page, so the
  // answerer repeats this rather than sharing a helper.
  const candidate = new Promise((resolve) => {
    pc.addEventListener('icecandidate', resolve, { once: true });
  });
  await pc.setLocalDescription();
  await candidate;
  return pc.localDescription.sdp;
}

async function answerer(offer) {
  const pc = new RTCPeerConnection();
  window.answerer = pc;
  await pc.setRemoteDescription({ type: 'offer', sdp: offer });
  const candidate = new Promise((resolve) => {
    pc.addEventListener('icecandidate', resolve, { once: true });
  });
  await pc.setLocalDescription();
  await candidate;
  return pc.localDescription.sdp;
}

test('a fresh offer and its answer round-trip byte for byte and still connect', async ({ page }) => {
  await page.goto('about:blank');
  const offer = await page.evaluate(offerer);
  const offerOut = roundTrip(offer);
  expect(offerOut).toBe(withCrlf(offer));

  const answer = await page.evaluate(answerer, offerOut);
  const answerOut = roundTrip(answer);
  expect(answerOut).toBe(withCrlf(answer));

  await page.evaluate((sdp) => window.offerer.setRemoteDescription({ type: 'answer', sdp }),
    answerOut);
  const states = () => [window.offerer, window.answerer]
    .map((pc) => `${pc.signalingState}/${pc.connectionState}`);
  await expect.poll(() => page.evaluate(states), { timeout: 30_000 })
    .toEqual(['stable/connected', 'stable/connected']);

  if (process.env.ULW_SDP_CAPTURE) {
    writeFileSync(path.join(fixtures, 'chromium_offer.sdp'), offer);
    writeFileSync(path.join(fixtures, 'chromium_answer.sdp'), answer);
  }
});

test('the committed offer and answer round-trip and Chromium accepts them', async ({ page }) => {
  await page.goto('about:blank');
  const offer = readFileSync(path.join(fixtures, 'chromium_offer.sdp'), 'utf8');
  const answer = readFileSync(path.join(fixtures, 'chromium_answer.sdp'), 'utf8');
  const offerOut = roundTrip(offer);
  const answerOut = roundTrip(answer);
  expect(offerOut).toBe(withCrlf(offer));
  expect(answerOut).toBe(withCrlf(answer));

  // The committed offer as the remote description of a fresh peer, which answers it; and the
  // committed answer as the remote description of a fresh peer whose own offer has the same
  // sections (audio, three-layer video, data) and so the same mids and rids.
  const states = await page.evaluate(async ([o, a]) => {
    const answering = new RTCPeerConnection();
    await answering.setRemoteDescription({ type: 'offer', sdp: o });
    await answering.setLocalDescription();
    const offering = new RTCPeerConnection();
    offering.addTransceiver('audio');
    offering.addTransceiver('video', {
      sendEncodings: [{ rid: 'q' }, { rid: 'h' }, { rid: 'f' }],
    });
    offering.createDataChannel('chat');
    await offering.setLocalDescription();
    await offering.setRemoteDescription({ type: 'answer', sdp: a });
    return [answering.signalingState, offering.signalingState];
  }, [offerOut, answerOut]);
  expect(states).toEqual(['stable', 'stable']);
});

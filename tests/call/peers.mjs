// What the call and ingest suites share: the signalling stand-in, the page server and the
// headless browsers.
import { chromium } from '@playwright/test';
import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import path from 'node:path';
import { createInterface } from 'node:readline';

const here = path.dirname(new URL(import.meta.url).pathname);
const harness = process.env.ULW_CALL_HARNESS;

// The call handler's stand-in: one harness process for the whole test, holding the rooms it
// opened as the handler would, driven one command line at a time.
export function startSignalling() {
  const child = spawn(harness, [], { stdio: ['pipe', 'pipe', 'inherit'] });
  const lines = createInterface({ input: child.stdout })[Symbol.asyncIterator]();
  const send = async (...words) => {
    child.stdin.write(`${words.join(' ')}\n`);
    const { value, done } = await lines.next();
    if (done) throw new Error(`harness exited during: ${words.join(' ')}`);
    if (value.startsWith('error')) throw new Error(`${words.join(' ')}: ${value}`);
    return value;
  };
  return {
    // A call admits its two members; a stream's room has no limit of its own, since its one
    // publisher is joined there by the recorder that relays it.
    open: (room, generation, max = 2) => send('open', room, generation, max),
    ticket: async (room, generation, user, device, role = 'member') =>
      JSON.parse(await send('join', room, generation, user, device, role)),
    relay: (room, generation, user, device, keyframeSeconds, url) =>
      send('relay', room, generation, user, device, keyframeSeconds, url),
    close: (room, generation) => send('close', room, generation),
    stop: () => child.stdin.end(),
  };
}

export function servePage() {
  const files = {
    '/': ['call.html', 'text/html'],
    '/livekit-client.umd.js': ['node_modules/livekit-client/dist/livekit-client.umd.js',
      'text/javascript'],
  };
  const server = createServer((req, res) => {
    const file = files[req.url];
    if (!file) {
      res.writeHead(404).end();
      return;
    }
    res.writeHead(200, { 'content-type': file[1] }).end(readFileSync(path.join(here, file[0])));
  });
  // Outside mode (run.sh) forwards a fixed port from the outside peer's loopback to this host,
  // so the page server listens there on every address; otherwise any loopback port will do.
  const port = Number(process.env.ULW_CALL_PAGE_PORT ?? 0);
  const address = port === 0 ? '127.0.0.1' : '0.0.0.0';
  return new Promise((resolve) => server.listen(port, address, () => resolve(server)));
}

// A browser server rather than a plain launch: only the server exposes its process, which the
// call suite's drop test has to freeze. An outside peer runs through run.sh's wrapper, in
// another network namespace.
export const outsideChrome = process.env.ULW_CALL_OUTSIDE_CHROME;

export async function launchPeer({ outside = false, args = [] } = {}) {
  const server = await chromium.launchServer({
    headless: true,
    executablePath: outside ? outsideChrome : process.env.ULW_E2E_CHROME,
    args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', ...args],
  });
  const browser = await chromium.connect(server.wsEndpoint());
  return { server, browser };
}

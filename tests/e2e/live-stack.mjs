// What one live E2E run needs, started from the build tree: the VOD run's gateway and page
// server (stack.mjs) on a bucket of its own, live_packager writing the stream into that bucket,
// and the test publisher feeding it. The player reaches the playlist only through the gateway's
// live route and the segments only through the URLs it signs; the bucket is private.
import { spawn } from 'node:child_process';
import { createHash, createHmac, randomBytes } from 'node:crypto';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { byCodePoint, config, startStack } from './stack.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const bin = (rel) => path.join(config.build, rel);

// A bucket of its own, so a run beside another checkout's cannot touch its objects.
export const bucket = process.env.ULW_LIVE_E2E_BUCKET ?? 'ulw-live-e2e';
// What the packager's SRT listener requires of its caller, as LiveKit egress would be given it
// in the stream URL.
const passphrase = 'e2e passphrase of 24 chars';

const encode = (text) => encodeURIComponent(text).replace(/[!'()*]/g, (c) =>
  `%${c.charCodeAt(0).toString(16).toUpperCase()}`);

// AWS Signature Version 4 for the clean-up the run does itself; the services sign their own.
async function s3(method, pathname, { query = {}, body = '' } = {}) {
  const endpoint = new URL(config.minio);
  const now = new Date().toISOString().replace(/[-:]/g, '').replace(/\.\d{3}/, '');
  const date = now.slice(0, 8);
  const region = 'us-east-1';
  const payload = createHash('sha256').update(body).digest('hex');
  const headers = { host: endpoint.host, 'x-amz-content-sha256': payload, 'x-amz-date': now };
  const signed = Object.keys(headers).sort(byCodePoint);
  const canonicalQuery = Object.keys(query).sort(byCodePoint)
    .map((k) => `${encode(k)}=${encode(query[k])}`).join('&');
  const canonicalHeaders = signed.map((h) => `${h}:${headers[h]}\n`).join('');
  const canonical = [method, pathname, canonicalQuery, canonicalHeaders, signed.join(';'),
    payload].join('\n');
  const scope = `${date}/${region}/s3/aws4_request`;
  const toSign = ['AWS4-HMAC-SHA256', now, scope,
    createHash('sha256').update(canonical).digest('hex')].join('\n');
  let key = `AWS4${config.secretKey}`;
  for (const part of [date, region, 's3', 'aws4_request']) {
    key = createHmac('sha256', key).update(part).digest();
  }
  headers.authorization = `AWS4-HMAC-SHA256 Credential=${config.accessKey}/${scope}, ` +
    `SignedHeaders=${signed.join(';')}, ` +
    `Signature=${createHmac('sha256', key).update(toSign).digest('hex')}`;
  const url = `${endpoint.origin}${pathname}${canonicalQuery ? `?${canonicalQuery}` : ''}`;
  return fetch(url, { method, headers, body: body === '' ? undefined : body });
}

async function removeStream(id) {
  const listing = await s3('GET', `/${bucket}`,
    { query: { 'list-type': '2', prefix: `live/${id}/` } });
  const keys = [...(await listing.text()).matchAll(/<Key>([^<]+)<\/Key>/g)].map((m) => m[1]);
  for (const key of keys) {
    await s3('DELETE', `/${bucket}/${key}`);
  }
}

class Process {
  constructor(name, file, args, env) {
    this.name = name;
    this.output = '';
    this.child = spawn(file, args, { env, stdio: ['ignore', 'pipe', 'pipe'] });
    this.exited = new Promise((resolve) => this.child.on('exit', (code, signal) =>
      resolve(code ?? 128 + (signal === 'SIGKILL' ? 9 : 15))));
    for (const stream of [this.child.stdout, this.child.stderr]) {
      stream.on('data', (chunk) => { this.output += chunk; });
    }
  }

  async waitFor(pattern, ms) {
    const deadline = Date.now() + ms;
    for (;;) {
      const found = this.output.match(pattern);
      if (found) {
        return found;
      }
      if (this.child.exitCode !== null || Date.now() > deadline) {
        throw new Error(`${this.name} never printed ${pattern}:\n${this.output}`);
      }
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
  }

  async stop() {
    if (this.child.exitCode === null) {
      this.child.kill('SIGTERM');
    }
    return this.exited;
  }
}

// `seconds` is how long the publisher runs; the packager ends the stream when it stops.
export async function startLiveStack({ seconds, segment = 2, window = 10 }) {
  const stack = await startStack({ player: 'live-player.html', bucket, worker: false });
  // An earlier version of this run opened the bucket to anonymous reads; nothing may now.
  await s3('DELETE', `/${bucket}`, { query: { policy: '' } });
  const work = mkdtempSync(path.join(tmpdir(), 'ulw-live-e2e-'));
  const id = `e2e-${randomBytes(6).toString('hex')}`;

  const packager = new Process('live_packager', bin('apps/live-packager/live_packager'), [], {
    PATH: process.env.PATH,
    ULW_STREAM_ID: id,
    ULW_LIVE_INGEST_PORT: '0',
    ULW_LIVE_SRT_PASSPHRASE: passphrase,
    ULW_LIVE_SEGMENT_SECONDS: String(segment),
    ULW_LIVE_WINDOW_SEGMENTS: String(window),
    ULW_STORAGE: 'minio',
    ULW_S3_ENDPOINT: config.minio,
    ULW_BUCKET: bucket,
    ULW_S3_ACCESS_KEY_ID: config.accessKey,
    ULW_S3_SECRET_ACCESS_KEY: config.secretKey,
    ULW_SCRATCH_DIR: work,
    ULW_SANDBOX_BIN: bin('apps/live-packager/ulw_sandbox'),
  });
  let port;
  try {
    [, port] = await packager.waitFor(/ingest=127\.0\.0\.1:(\d+)/, 30_000);
  } catch (e) {
    await packager.stop();
    await stack.stop();
    throw e;
  }
  const route = `/api/v1/live/${id}/index.m3u8`;

  return {
    id,
    token: stack.token,
    // What the player loads: the gateway's route on the page's own origin, as behind the
    // Askedin route.
    manifest: `${stack.origin}${route}`,
    // The same route asked of the gateway directly, for the run's own observer.
    gatewayManifest: `${stack.gateway}${route}`,
    gateway: stack.gateway,
    origin: stack.origin,
    storageOrigin: stack.storageOrigin,
    proxied: stack.proxied,
    gatewayOutput: stack.gatewayOutput,
    packager,
    // Starts the publisher: the moment the stream begins.
    publish() {
      const publisher = new Process('ulw-live-testsource',
        path.join(root, 'apps/live-packager/testsource/ulw-live-testsource'),
        [`127.0.0.1:${port}`, String(seconds)],
        { PATH: process.env.PATH, ULW_TESTSOURCE_SEGMENT: String(segment),
          ULW_TESTSOURCE_PASSPHRASE: passphrase, ULW_TESTSOURCE_STREAMID: id });
      this.publisher = publisher;
      return publisher;
    },
    async stop() {
      try {
        if (this.publisher) {
          await this.publisher.stop();
        }
        const code = await packager.stop();
        await removeStream(id);
        rmSync(work, { recursive: true, force: true });
        return code;
      } finally {
        await stack.stop();
      }
    },
  };
}

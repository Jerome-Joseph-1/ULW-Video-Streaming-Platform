// The services one E2E run needs, started from the build tree: a scratch database on the
// local Postgres, a bucket on the local MinIO, gateway_server and (for VOD) transcode_worker,
// and a page server that stands in for an operator's route putting /api on the app's own origin.
import { spawn, execFileSync } from 'node:child_process';
import { createHash, createHmac, randomBytes } from 'node:crypto';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');

export const config = {
  build: path.resolve(root, process.env.ULW_BUILD_DIR ?? 'build/ci'),
  postgres: process.env.ULW_TEST_DATABASE_URL ??
    'postgresql://postgres:testtest123@127.0.0.1:55432/postgres',
  minio: process.env.ULW_MINIO_ENDPOINT ?? 'http://127.0.0.1:9000',
  accessKey: process.env.ULW_MINIO_ACCESS_KEY ?? 'ulw-dev',
  secretKey: process.env.ULW_MINIO_SECRET_KEY ?? 'ulw-dev-secret',
  bucket: 'ulw-e2e',
  issuer: 'ulw-e2e',
};

const bin = (rel) => path.join(config.build, rel);

function withDatabase(url, name) {
  const u = new URL(url);
  u.pathname = `/${name}`;
  return u.toString();
}

// SigV4 sorts header and query names by their bytes (AWS's signing spec), not by locale:
// localeCompare can order '-', '_' and '~' against letters and digits differently, and the
// signature would then not match. The names are ASCII, whose UTF-16 code units compare as the
// bytes do.
export const byCodePoint = (a, b) => Number(a > b) - Number(a < b);

function psql(url, sql) {
  execFileSync('psql', ['-v', 'ON_ERROR_STOP=1', '-q', '-d', url, '-c', sql], { stdio: 'pipe' });
}

async function freePort() {
  const server = net.createServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address();
  await new Promise((resolve) => server.close(resolve));
  return port;
}

// AWS Signature Version 4 for the one call the stack makes itself, creating its bucket; the
// services sign everything else.
function signedBucketRequest(method, bucket) {
  const endpoint = new URL(config.minio);
  const now = new Date().toISOString().replace(/[-:]/g, '').replace(/\.\d{3}/, '');
  const date = now.slice(0, 8);
  const region = 'us-east-1';
  const payload = createHash('sha256').update('').digest('hex');
  const headers = { host: endpoint.host, 'x-amz-content-sha256': payload, 'x-amz-date': now };
  const signed = Object.keys(headers).sort(byCodePoint);
  const canonicalHeaders = signed.map((h) => `${h}:${headers[h]}\n`).join('');
  const canonical = `${method}\n/${bucket}\n\n${canonicalHeaders}\n${signed.join(';')}\n${payload}`;
  const scope = `${date}/${region}/s3/aws4_request`;
  const toSign = ['AWS4-HMAC-SHA256', now, scope,
    createHash('sha256').update(canonical).digest('hex')].join('\n');
  let key = `AWS4${config.secretKey}`;
  for (const part of [date, region, 's3', 'aws4_request']) {
    key = createHmac('sha256', key).update(part).digest();
  }
  const signature = createHmac('sha256', key).update(toSign).digest('hex');
  headers.authorization = `AWS4-HMAC-SHA256 Credential=${config.accessKey}/${scope}, ` +
    `SignedHeaders=${signed.join(';')}, Signature=${signature}`;
  return { url: `${endpoint.origin}/${bucket}`, init: { method, headers } };
}

async function ensureBucket(bucket) {
  const { url, init } = signedBucketRequest('PUT', bucket);
  const r = await fetch(url, init);
  const body = await r.text();
  if (!r.ok && !body.includes('BucketAlreadyOwnedByYou')) {
    throw new Error(`cannot create bucket ${bucket}: ${r.status} ${body}`);
  }
}

class Service {
  constructor(name, file, env) {
    this.name = name;
    this.output = '';
    this.child = spawn(file, [], { env, stdio: ['ignore', 'pipe', 'pipe'] });
    this.exited = new Promise((resolve) => this.child.on('exit', (code, signal) =>
      resolve(code ?? 128 + (signal === 'SIGKILL' ? 9 : 15))));
    for (const stream of [this.child.stdout, this.child.stderr]) {
      stream.on('data', (chunk) => { this.output += chunk; });
    }
  }

  async waitFor(text, ms) {
    const deadline = Date.now() + ms;
    while (!this.output.includes(text)) {
      if (this.child.exitCode !== null || Date.now() > deadline) {
        throw new Error(`${this.name} never printed "${text}":\n${this.output}`);
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

// Serves the player page and hls.js from node_modules, and passes /api through to the gateway,
// noting every path it passed. Nothing else is served, so a segment asked of this origin fails.
function pageServer(gatewayPort, proxied, player) {
  const files = {
    '/': ['text/html', path.join(here, player)],
    // A broadcaster's page, for the live publishing run.
    '/publisher': ['text/html', path.join(here, 'live-publisher.html')],
    '/hls.js': ['text/javascript', path.join(here, 'node_modules/hls.js/dist/hls.min.js')],
  };
  return http.createServer((req, res) => {
    const pathname = new URL(req.url, 'http://page').pathname;
    if (pathname.startsWith('/api/')) {
      proxied.push(pathname);
      const upstream = http.request({ host: '127.0.0.1', port: gatewayPort, path: req.url,
        method: req.method, headers: req.headers }, (answer) => {
        res.writeHead(answer.statusCode, answer.headers);
        answer.pipe(res);
      });
      upstream.on('error', () => { res.writeHead(502); res.end(); });
      req.pipe(upstream);
      return;
    }
    const file = files[pathname];
    if (!file) {
      res.writeHead(404);
      res.end();
      return;
    }
    res.writeHead(200, { 'content-type': file[0] });
    res.end(readFileSync(file[1]));
  });
}

// `player` is the page served at /; the live run plays with its own page and bucket, and
// needs no worker. `gatewayEnv` adds to the gateway's environment, given the stack's work
// directory and database; `users` are further subjects to mint tokens for.
export async function startStack({ player = 'player.html', bucket = config.bucket,
  worker: withWorker = true, gatewayEnv = () => ({}), users = [] } = {}) {
  const work = mkdtempSync(path.join(tmpdir(), 'ulw-e2e-'));
  const database = `ulw_e2e_${randomBytes(6).toString('hex')}`;
  const databaseUrl = withDatabase(config.postgres, database);
  psql(config.postgres, `CREATE DATABASE ${database}`);
  // A developer's container may run the suite as root, which the services refuse unless told.
  const rootAllowed = { ULW_ALLOW_ROOT: '1' };
  execFileSync(bin('apps/migrate/ulw_migrate'), [], {
    env: { ...process.env, ULW_DATABASE_URL: databaseUrl, ...rootAllowed }, stdio: 'pipe' });
  await ensureBucket(bucket);

  const key = path.join(work, 'dev-key.json');
  execFileSync(bin('tools/devtoken/ulw_devtoken'), ['keygen', key]);
  const jwks = path.join(work, 'jwks.json');
  writeFileSync(jwks, execFileSync(bin('tools/devtoken/ulw_devtoken'), ['jwks', key]));
  const mint = (sub) => execFileSync(bin('tools/devtoken/ulw_devtoken'),
    ['mint', key, '--iss', config.issuer, '--sub', sub, '--ttl', '3600']).toString().trim();
  const token = mint('e2e-viewer');
  const tokens = Object.fromEntries(users.map((sub) => [sub, mint(sub)]));

  const storage = {
    ULW_STORAGE: 'minio',
    ULW_S3_ENDPOINT: config.minio,
    ULW_BUCKET: bucket,
    ULW_S3_ACCESS_KEY_ID: config.accessKey,
    ULW_S3_SECRET_ACCESS_KEY: config.secretKey,
    ULW_DATABASE_URL: databaseUrl,
  };
  const gatewayPort = await freePort();
  const gateway = new Service('gateway_server', bin('apps/gateway/gateway_server'), {
    PATH: process.env.PATH,
    ...storage,
    ...(process.env.ULW_REACTOR ? { ULW_REACTOR: process.env.ULW_REACTOR } : {}),
    ULW_LISTEN_PORT: String(gatewayPort),
    ULW_DEV_JWKS_FILE: jwks,
    ULW_DEV_MODE: "1",
    JWT_ISSUER: config.issuer,
    ...gatewayEnv({ work, databaseUrl }),
    // The per-client limits stay at their defaults: a real browser playing through them is
    // part of what this suite shows.
    ...rootAllowed,
  });
  const worker = withWorker ? new Service('transcode_worker', bin('apps/worker/transcode_worker'), {
    PATH: process.env.PATH,
    ...storage,
    ULW_NODE_ID: 'e2e-worker',
    ULW_SCRATCH_DIR: work,
    ULW_SANDBOX_BIN: bin('apps/worker/ulw_sandbox'),
    ...rootAllowed,
  }) : null;
  await gateway.waitFor(`"port":${gatewayPort}`, 30_000);

  const proxied = [];
  const page = pageServer(gatewayPort, proxied, player);
  await new Promise((resolve) => page.listen(0, '127.0.0.1', resolve));

  return {
    token,
    tokens,
    gateway: `http://127.0.0.1:${gatewayPort}`,
    gatewayPort,
    origin: `http://127.0.0.1:${page.address().port}`,
    storageOrigin: new URL(config.minio).origin,
    proxied,
    // What gateway_server has logged so far, for a failure report.
    gatewayOutput: () => gateway.output,
    workerOutput: () => (worker ? worker.output : ''),
    work,
    async stop() {
      await new Promise((resolve) => page.close(resolve));
      const codes = [await gateway.stop(), worker ? await worker.stop() : 0];
      psql(config.postgres, `DROP DATABASE IF EXISTS ${database} WITH (FORCE)`);
      rmSync(work, { recursive: true, force: true });
      if (codes.some((c) => c !== 0)) {
        throw new Error(`a service exited uncleanly (${codes}):\n${gateway.output}\n` +
          (worker ? worker.output : ''));
      }
    },
  };
}

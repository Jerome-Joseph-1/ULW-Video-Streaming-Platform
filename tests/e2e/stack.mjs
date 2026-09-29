// The services one E2E run needs, started from the build tree: a scratch database on the
// local Postgres, a bucket on the local MinIO, gateway_server and transcode_worker, and a page
// server that stands in for the Askedin route putting /api on the app's own origin.
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
  const signed = Object.keys(headers).sort();
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

async function ensureBucket() {
  const { url, init } = signedBucketRequest('PUT', config.bucket);
  const r = await fetch(url, init);
  const body = await r.text();
  if (!r.ok && !body.includes('BucketAlreadyOwnedByYou')) {
    throw new Error(`cannot create bucket ${config.bucket}: ${r.status} ${body}`);
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
function pageServer(gatewayPort, proxied) {
  const files = {
    '/': ['text/html', path.join(here, 'player.html')],
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

export async function startStack() {
  const work = mkdtempSync(path.join(tmpdir(), 'ulw-e2e-'));
  const database = `ulw_e2e_${randomBytes(6).toString('hex')}`;
  const databaseUrl = withDatabase(config.postgres, database);
  psql(config.postgres, `CREATE DATABASE ${database}`);
  execFileSync(bin('apps/migrate/ulw_migrate'), [], {
    env: { ...process.env, ULW_DATABASE_URL: databaseUrl }, stdio: 'pipe' });
  await ensureBucket();

  const key = path.join(work, 'dev-key.json');
  execFileSync(bin('tools/devtoken/ulw_devtoken'), ['keygen', key]);
  const jwks = path.join(work, 'jwks.json');
  writeFileSync(jwks, execFileSync(bin('tools/devtoken/ulw_devtoken'), ['jwks', key]));
  const token = execFileSync(bin('tools/devtoken/ulw_devtoken'),
    ['mint', key, '--iss', config.issuer, '--sub', 'e2e-viewer', '--ttl', '3600'])
    .toString().trim();

  const storage = {
    ULW_STORAGE: 'minio',
    ULW_S3_ENDPOINT: config.minio,
    ULW_BUCKET: config.bucket,
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
    JWT_ISSUER: config.issuer,
  });
  const worker = new Service('transcode_worker', bin('apps/worker/transcode_worker'), {
    PATH: process.env.PATH,
    ...storage,
    ULW_NODE_ID: 'e2e-worker',
    ULW_SCRATCH_DIR: work,
    ULW_SANDBOX_BIN: bin('apps/worker/ulw_sandbox'),
  });
  await gateway.waitFor(`port=${gatewayPort}`, 30_000);

  const proxied = [];
  const page = pageServer(gatewayPort, proxied);
  await new Promise((resolve) => page.listen(0, '127.0.0.1', resolve));

  return {
    token,
    gateway: `http://127.0.0.1:${gatewayPort}`,
    gatewayPort,
    origin: `http://127.0.0.1:${page.address().port}`,
    storageOrigin: new URL(config.minio).origin,
    proxied,
    work,
    async stop() {
      await new Promise((resolve) => page.close(resolve));
      const codes = [await gateway.stop(), await worker.stop()];
      psql(config.postgres, `DROP DATABASE IF EXISTS ${database} WITH (FORCE)`);
      rmSync(work, { recursive: true, force: true });
      if (codes.some((c) => c !== 0)) {
        throw new Error(`a service exited uncleanly (${codes}):\n${gateway.output}\n` +
          worker.output);
      }
    },
  };
}

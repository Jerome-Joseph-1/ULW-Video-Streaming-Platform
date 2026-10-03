// The product side of a call, as a deployment runs it: chat_server nodes (two by default) of one cluster on a
// scratch database of the local Postgres, configured with the LiveKit run.sh points the suite at,
// and tokens minted with the build's ulw_devtoken under a key the nodes trust. What
// direct-call.spec.mjs runs against; no ticket comes from anywhere but chat.
import { execFileSync, spawn } from 'node:child_process';
import { randomBytes, randomInt } from 'node:crypto';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import net from 'node:net';
import { tmpdir } from 'node:os';
import path from 'node:path';

const here = path.dirname(new URL(import.meta.url).pathname);
const build = path.resolve(here, '../..', process.env.ULW_BUILD_DIR ?? 'build/ci');
const bin = (rel) => path.join(build, rel);
const postgres = process.env.ULW_TEST_DATABASE_URL ??
  'postgresql://postgres:testtest123@127.0.0.1:55432/postgres';
const issuer = 'ulw-call-e2e';

function withDatabase(url, name) {
  const u = new URL(url);
  u.pathname = `/${name}`;
  return u.toString();
}

export function psql(url, sql) {
  return execFileSync('psql', ['-v', 'ON_ERROR_STOP=1', '-qtA', '-d', url, '-c', sql],
    { stdio: 'pipe' }).toString().trim();
}

async function bindable(port, host) {
  const server = net.createServer();
  const ok = await new Promise((resolve) => {
    server.once('error', () => resolve(false));
    server.listen(port, host, () => resolve(true));
  });
  if (ok) await new Promise((resolve) => server.close(resolve));
  return ok;
}

// A port nothing holds now, on loopback and the wildcard address, below the local port range: as
// tests/support/reserve_port.hpp explains, a port there can never be some outgoing connection's
// source port by the time a node binds it. Never one this process handed out before.
const handedOut = new Set();
async function freePort() {
  const [low] = readFileSync('/proc/sys/net/ipv4/ip_local_port_range', 'utf8').trim().split(/\s+/)
    .map(Number);
  const first = 20_000;
  const end = low > first + 1000 ? low - 1000 : 32_768;
  for (let tried = 0; tried < 1000; ++tried) {
    const port = first + randomInt(end - first);
    if (handedOut.has(port)) continue;
    if (await bindable(port, '127.0.0.1') && await bindable(port, '0.0.0.0')) {
      handedOut.add(port);
      return port;
    }
  }
  throw new Error('no free port');
}

class Node {
  constructor(name, env) {
    this.name = name;
    this.output = '';
    this.child = spawn(bin('apps/chat/chat_server'), [],
      { env, stdio: ['ignore', 'pipe', 'pipe'] });
    this.exited = new Promise((resolve) => this.child.on('exit', (code, signal) =>
      resolve(code ?? `signal ${signal}`)));
    for (const stream of [this.child.stdout, this.child.stderr]) {
      stream.on('data', (chunk) => { this.output += chunk; });
    }
  }

  async stop() {
    if (this.child.exitCode === null && this.child.signalCode === null) this.child.kill('SIGTERM');
    return this.exited;
  }

  // As a node failure would end it: no drain, no release of its rooms.
  async kill() {
    this.killed = true;
    this.child.kill('SIGKILL');
    return this.exited;
  }
}

// Polls `check` every 50 ms until it holds, or throws with `what` after `ms`.
async function until(check, ms, what) {
  const deadline = Date.now() + ms;
  for (;;) {
    if (await check()) return;
    if (Date.now() > deadline) throw new Error(`${what} within ${ms} ms`);
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
}

// `origin` is the page's: the browsers authenticate with the cookie, which chat takes only from
// a listed origin (docs/integration/chat.md).
// `env` adds to the nodes' environment.
export async function startChat({ origin, nodes: count = 2, env: extra = {} }) {
  const livekit = Object.fromEntries(['LIVEKIT_API_URL', 'LIVEKIT_CLIENT_URL', 'LIVEKIT_API_KEY',
    'LIVEKIT_API_SECRET'].map((name) => {
    if (!process.env[name]) throw new Error(`${name} is not set; run through run.sh`);
    return [name, process.env[name]];
  }));
  const work = mkdtempSync(path.join(tmpdir(), 'ulw-call-chat-'));
  const database = `ulw_call_${randomBytes(6).toString('hex')}`;
  const databaseUrl = withDatabase(postgres, database);
  psql(postgres, `CREATE DATABASE ${database}`);
  const nodes = [];
  const stop = async () => {
    const codes = await Promise.all(nodes.map((n) => n.stop()));
    psql(postgres, `DROP DATABASE IF EXISTS ${database} WITH (FORCE)`);
    rmSync(work, { recursive: true, force: true });
    if (codes.some((c, i) => c !== 0 && !nodes[i].killed)) {
      throw new Error(`a chat node exited uncleanly (${codes}):\n` +
        nodes.map((n) => n.output).join('\n'));
    }
  };
  try {
    // A developer's container may run the suite as root, which the services refuse unless told.
    const rootAllowed = { ULW_ALLOW_ROOT: '1' };
    execFileSync(bin('apps/migrate/ulw_migrate'), [], {
      env: { ...process.env, ULW_DATABASE_URL: databaseUrl, ...rootAllowed }, stdio: 'pipe' });

    const devtoken = bin('tools/devtoken/ulw_devtoken');
    const key = path.join(work, 'dev-key.json');
    execFileSync(devtoken, ['keygen', key]);
    const jwks = path.join(work, 'jwks.json');
    writeFileSync(jwks, execFileSync(devtoken, ['jwks', key]));
    const mint = (sub) => execFileSync(devtoken,
      ['mint', key, '--iss', issuer, '--sub', sub, '--ttl', '3600']).toString().trim();

    const secret = randomBytes(32).toString('hex');
    const env = {
      PATH: process.env.PATH,
      ...(process.env.ULW_REACTOR ? { ULW_REACTOR: process.env.ULW_REACTOR } : {}),
      ULW_DEV_LOOPBACK_NODES: '1',
      ULW_NODE_SECRET: randomBytes(32).toString('hex'),
      ULW_DATABASE_URL: databaseUrl,
      ULW_DEV_JWKS_FILE: jwks,
      ULW_DEV_MODE: '1',
      JWT_ISSUER: issuer,
      ULW_ALLOWED_ORIGINS: origin,
      ...livekit,
      ...rootAllowed,
      ...extra,
    };
    for (let i = 0; i < count; ++i) {
      // Another process may take a port between its pick and the node's bind: then the node
      // exits saying so, and is started again on new ones.
      for (let attempt = 1; ; ++attempt) {
        const port = await freePort();
        const node = new Node(`chat-${i + 1}`, { ...env, ULW_NODE_ID: `chat-${i + 1}`,
          ULW_LISTEN_PORT: String(port), ULW_NODE_ADDRESS: `127.0.0.1:${await freePort()}` });
        node.port = port;
        node.ws = `ws://127.0.0.1:${port}/rt`;
        await until(() => {
          if (node.child.exitCode !== null) return true;
          return node.output.includes('"msg":"listening"');
        }, 30_000, `${node.name} never listened`);
        if (node.child.exitCode === null) {
          nodes.push(node);
          break;
        }
        if (attempt === 5 || !node.output.includes('Address already in use')) {
          throw new Error(`${node.name} exited:\n${node.output}`);
        }
      }
    }
    for (const node of nodes) {
      await until(async () => {
        if (node.child.exitCode !== null) throw new Error(`${node.name} exited:\n${node.output}`);
        const r = await fetch(`http://127.0.0.1:${node.port}/readyz`).catch(() => null);
        return r?.status === 200;
      }, 30_000, `${node.name} was not ready`);
    }
    return {
      nodes,
      databaseUrl,
      mint,
      // A counter from a node's /metrics.
      async metric(node, name) {
        const text = await (await fetch(`http://127.0.0.1:${node.port}/metrics`)).text();
        const line = text.split('\n').find((l) => l.startsWith(`${name} `));
        if (line === undefined) throw new Error(`${node.name} has no ${name}`);
        return Number(line.slice(name.length + 1));
      },
      output: () => nodes.map((n) => `--- ${n.name}\n${n.output}`).join('\n'),
      stop,
    };
  } catch (e) {
    await stop().catch(() => {});
    throw e;
  }
}

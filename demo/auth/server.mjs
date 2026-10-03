// The demo's stand-in for an identity provider, and its one-time setup. Node's standard library
// only, so the stock node image runs it with nothing installed.
//
// At start it:
//   - makes (once) an Ed25519 signing key in /keys, and writes its public JWK set to
//     /keys/jwks.json, the file the gateway and chat verify tokens with (ULW_DEV_JWKS_FILE,
//     allowed only with ULW_DEV_MODE=1; docs/integration/auth.md), as tools/devtoken does;
//   - creates the demo's bucket on MinIO (signed S3 PUT), waiting for MinIO to answer.
// Then it serves, on 127.0.0.1:8070 behind the web proxy's /auth/:
//   GET  /auth/healthz            200 once the key set and the bucket exist
//   GET  /auth/users              the demo users the picker offers
//   POST /auth/token?sub=alice    {"token": ...}, a 12 hour token for that demo user
//
// Demo only: anyone who can reach the page can mint a token for any demo user. The key lives in
// the demo's own Docker volume and never leaves the machine.
import { createHash, createHmac, createPrivateKey, createPublicKey, generateKeyPairSync, sign }
  from 'node:crypto';
import { existsSync, readFileSync, writeFileSync, renameSync } from 'node:fs';
import http from 'node:http';

const env = (name, fallback) => process.env[name] ?? fallback;
const keyDir = env('DEMO_KEY_DIR', '/keys');
const issuer = env('JWT_ISSUER', 'ulw-demo');
const audience = env('JWT_AUDIENCE', 'ulw-dev');
const users = env('DEMO_USERS', 'alice,bob,carol').split(',');
const ttl = Number(env('DEMO_TOKEN_TTL', String(12 * 3600)));
const s3 = {
  endpoint: new URL(env('ULW_S3_ENDPOINT', 'http://127.0.0.1:9900')),
  bucket: env('ULW_BUCKET', 'ulw-demo'),
  accessKey: env('ULW_S3_ACCESS_KEY_ID', 'ulw-demo'),
  secretKey: env('ULW_S3_SECRET_ACCESS_KEY', 'ulw-demo-testtest123'),
};

const b64url = (buf) => Buffer.from(buf).toString('base64url');

function loadKey() {
  const file = `${keyDir}/dev-key.json`;
  if (!existsSync(file)) {
    const { privateKey } = generateKeyPairSync('ed25519');
    writeFileSync(`${file}.tmp`, JSON.stringify(privateKey.export({ format: 'jwk' })),
      { mode: 0o600 });
    renameSync(`${file}.tmp`, file);
  }
  const privateKey = createPrivateKey({ key: JSON.parse(readFileSync(file, 'utf8')), format: 'jwk' });
  const { x } = createPublicKey(privateKey).export({ format: 'jwk' });
  // RFC 7638 thumbprint, as ulw_devtoken derives the kid.
  const kid = b64url(createHash('sha256').update(`{"crv":"Ed25519","kty":"OKP","x":"${x}"}`).digest());
  const jwks = { keys: [{ kty: 'OKP', crv: 'Ed25519', use: 'sig', alg: 'EdDSA', kid, x }] };
  writeFileSync(`${keyDir}/jwks.json.tmp`, `${JSON.stringify(jwks)}\n`, { mode: 0o644 });
  renameSync(`${keyDir}/jwks.json.tmp`, `${keyDir}/jwks.json`);
  return { privateKey, kid };
}

function mint({ privateKey, kid }, sub) {
  const now = Math.floor(Date.now() / 1000);
  const header = b64url(JSON.stringify({ alg: 'EdDSA', typ: 'JWT', kid }));
  const payload = b64url(JSON.stringify({ iss: issuer, aud: audience, sub,
    email: `${sub}@demo.invalid`, iat: now, exp: now + ttl }));
  const input = `${header}.${payload}`;
  return `${input}.${b64url(sign(null, Buffer.from(input), privateKey))}`;
}

// AWS Signature Version 4, for the one bucket request the setup makes.
async function s3Request(method, path) {
  const now = new Date().toISOString().replace(/[-:]/g, '').replace(/\.\d{3}/, '');
  const date = now.slice(0, 8);
  const region = 'us-east-1';
  const payload = createHash('sha256').update('').digest('hex');
  const headers = { host: s3.endpoint.host, 'x-amz-content-sha256': payload, 'x-amz-date': now };
  const signed = Object.keys(headers).sort();
  const canonical = [method, path, '', signed.map((h) => `${h}:${headers[h]}\n`).join(''),
    signed.join(';'), payload].join('\n');
  const scope = `${date}/${region}/s3/aws4_request`;
  const toSign = ['AWS4-HMAC-SHA256', now, scope,
    createHash('sha256').update(canonical).digest('hex')].join('\n');
  let key = `AWS4${s3.secretKey}`;
  for (const part of [date, region, 's3', 'aws4_request']) {
    key = createHmac('sha256', key).update(part).digest();
  }
  headers.authorization = `AWS4-HMAC-SHA256 Credential=${s3.accessKey}/${scope}, ` +
    `SignedHeaders=${signed.join(';')}, ` +
    `Signature=${createHmac('sha256', key).update(toSign).digest('hex')}`;
  return fetch(`${s3.endpoint.origin}${path}`, { method, headers });
}

async function ensureBucket() {
  for (let attempt = 1; ; ++attempt) {
    try {
      const head = await s3Request('HEAD', `/${s3.bucket}`);
      if (head.status === 200) return;
      const made = await s3Request('PUT', `/${s3.bucket}`);
      if (made.status === 200) {
        console.log(`bucket ${s3.bucket} created`);
        return;
      }
      console.log(`bucket ${s3.bucket}: HEAD ${head.status}, PUT ${made.status}; retrying`);
    } catch (e) {
      if (attempt % 10 === 1) console.log(`waiting for MinIO at ${s3.endpoint.origin}: ${e.cause?.code ?? e.message}`);
    }
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
}

const key = loadKey();
console.log(`key set written to ${keyDir}/jwks.json (kid ${key.kid}); issuer ${issuer}`);
let ready = false;
ensureBucket().then(() => { ready = true; console.log('ready'); });

const send = (res, status, body) => {
  res.writeHead(status, { 'content-type': 'application/json', 'cache-control': 'no-store' });
  res.end(JSON.stringify(body));
};

http.createServer((req, res) => {
  const url = new URL(req.url, 'http://auth');
  if (req.method === 'GET' && url.pathname === '/auth/healthz') {
    return send(res, ready ? 200 : 503, { ready });
  }
  if (req.method === 'GET' && url.pathname === '/auth/users') {
    return send(res, 200, { users });
  }
  if (req.method === 'POST' && url.pathname === '/auth/token') {
    const sub = url.searchParams.get('sub') ?? '';
    if (!users.includes(sub)) return send(res, 400, { error: `sub must be one of ${users}` });
    return send(res, 200, { token: mint(key, sub), user: sub, expires_in: ttl });
  }
  return send(res, 404, { error: 'not found' });
}).listen(Number(env('DEMO_AUTH_PORT', '8070')), env('DEMO_AUTH_HOST', '127.0.0.1'));

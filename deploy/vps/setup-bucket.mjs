// One-time setup for deploy/vps/compose.yaml: creates the platform's bucket on MinIO, waiting for
// MinIO to answer, and exits 0 once it exists (idempotent). Node's standard library only.
import { createHash, createHmac } from 'node:crypto';

const env = (name) => {
  const v = process.env[name];
  if (!v) { console.error(`${name} is not set`); process.exit(2); }
  return v;
};
const endpoint = new URL(env('ULW_S3_ENDPOINT'));
const bucket = env('ULW_BUCKET');
const accessKey = env('ULW_S3_ACCESS_KEY_ID');
const secretKey = env('ULW_S3_SECRET_ACCESS_KEY');

// AWS Signature Version 4 for a bodiless bucket request.
function request(method, path) {
  const now = new Date().toISOString().replace(/[-:]/g, '').replace(/\.\d{3}/, '');
  const date = now.slice(0, 8);
  const region = 'us-east-1';
  const payload = createHash('sha256').update('').digest('hex');
  const headers = { host: endpoint.host, 'x-amz-content-sha256': payload, 'x-amz-date': now };
  const signed = Object.keys(headers).sort();
  const canonical = [method, path, '', signed.map((h) => `${h}:${headers[h]}\n`).join(''),
    signed.join(';'), payload].join('\n');
  const scope = `${date}/${region}/s3/aws4_request`;
  const toSign = ['AWS4-HMAC-SHA256', now, scope, createHash('sha256').update(canonical).digest('hex')].join('\n');
  let key = `AWS4${secretKey}`;
  for (const part of [date, region, 's3', 'aws4_request']) key = createHmac('sha256', key).update(part).digest();
  headers.authorization = `AWS4-HMAC-SHA256 Credential=${accessKey}/${scope}, SignedHeaders=${signed.join(';')}, ` +
    `Signature=${createHmac('sha256', key).update(toSign).digest('hex')}`;
  return fetch(`${endpoint.origin}${path}`, { method, headers });
}

for (let attempt = 1; attempt <= 120; ++attempt) {
  try {
    const head = await request('HEAD', `/${bucket}`);
    if (head.status === 200) { console.log(`bucket ${bucket} exists`); process.exit(0); }
    const made = await request('PUT', `/${bucket}`);
    if (made.status === 200) { console.log(`bucket ${bucket} created`); process.exit(0); }
    console.log(`bucket ${bucket}: HEAD ${head.status}, PUT ${made.status}; retrying`);
  } catch (e) {
    console.log(`waiting for MinIO at ${endpoint.origin}: ${e.cause?.code ?? e.message}`);
  }
  await new Promise((resolve) => setTimeout(resolve, 1000));
}
console.error(`bucket ${bucket}: MinIO did not answer`);
process.exit(1);

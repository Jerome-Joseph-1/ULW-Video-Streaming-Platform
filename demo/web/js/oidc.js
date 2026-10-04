// Sign-in with an OpenID Connect provider (config.js's `auth: "oidc"`): the authorization code
// flow with PKCE (RFC 7636, S256) for a public client, written against WebCrypto and fetch so
// the page has no dependency for it. The tokens live in this window's sessionStorage only; the
// access token goes to the gateway as the bearer header and to chat as the WebSocket's ?token=
// (core.js), and is renewed with the refresh token a minute before it expires.
//
// Each function takes core.js's oidcConfig: { issuer, clientId, userClaim }, from config.js's
// oidcIssuer, oidcClientId and oidcUserClaim (build/web-config.sh; deploy/vps/deploy.sh).

const PENDING = 'ulw-oidc:pending';
const TOKENS = 'ulw-oidc:tokens';

const b64url = (bytes) => btoa(String.fromCharCode(...bytes)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
const random = (n = 32) => b64url(crypto.getRandomValues(new Uint8Array(n)));

export function claims(jwt) {
  const part = jwt.split('.')[1].replace(/-/g, '+').replace(/_/g, '/');
  const text = new TextDecoder().decode(Uint8Array.from(atob(part), (c) => c.charCodeAt(0)));
  return JSON.parse(text);
}

const read = (key) => { try { return JSON.parse(sessionStorage.getItem(key)); } catch { return null; } };
const write = (key, value) => {
  try {
    if (value === null) sessionStorage.removeItem(key);
    else sessionStorage.setItem(key, JSON.stringify(value));
  } catch { /* private window: this page load only */ }
};

let metadata = null;
async function discover(cfg) {
  if (metadata) return metadata;
  const r = await fetch(`${cfg.issuer}/.well-known/openid-configuration`, { cache: 'no-store' });
  if (!r.ok) throw new Error(`the identity provider answered ${r.status}`);
  const m = await r.json();
  if (m.issuer !== cfg.issuer) throw new Error(`the identity provider names itself ${m.issuer}, not ${cfg.issuer}`);
  metadata = m;
  return m;
}

const redirectUri = (cfg) => cfg.redirectUri ?? `${location.origin}/`;

// Leaves the page for the provider's sign-in; it comes back to redirectUri with ?code&state.
export async function login(cfg, { prompt } = {}) {
  const m = await discover(cfg);
  const verifier = random(48);
  const challenge = b64url(new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(verifier))));
  const state = random();
  const nonce = random();
  const back = new URLSearchParams(location.search);
  for (const k of ['code', 'state', 'session_state', 'iss', 'error', 'error_description']) back.delete(k);
  write(PENDING, { verifier, state, nonce, back: back.toString() });
  const params = new URLSearchParams({
    response_type: 'code', client_id: cfg.clientId, redirect_uri: redirectUri(cfg),
    scope: cfg.scope ?? 'openid profile', state, nonce,
    code_challenge: challenge, code_challenge_method: 'S256',
  });
  if (prompt) params.set('prompt', prompt);
  location.assign(`${m.authorization_endpoint}?${params}`);
  return new Promise(() => {}); // the page is leaving
}

function save(body) {
  const tokens = {
    access: body.access_token,
    refresh: body.refresh_token ?? read(TOKENS)?.refresh ?? null,
    id: body.id_token ?? read(TOKENS)?.id ?? null,
    expiresAt: Date.now() + (Number(body.expires_in) || 300) * 1000,
  };
  write(TOKENS, tokens);
  return tokens;
}

async function tokenRequest(cfg, form) {
  const m = await discover(cfg);
  const r = await fetch(m.token_endpoint, {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({ client_id: cfg.clientId, ...form }),
  });
  const body = await r.json().catch(() => ({}));
  if (!r.ok || typeof body.access_token !== 'string') {
    throw new Error(`token endpoint: ${r.status} ${body.error ?? ''}`.trim());
  }
  return body;
}

// On the way back from the provider: trades the code for tokens and tidies the address bar.
// true when this load was such a return.
export async function complete(cfg) {
  const params = new URLSearchParams(location.search);
  if (!params.has('state') || !(params.has('code') || params.has('error'))) return false;
  const pending = read(PENDING);
  write(PENDING, null);
  history.replaceState(null, '', `${location.pathname}${pending?.back ? `?${pending.back}` : ''}`);
  if (!pending || params.get('state') !== pending.state) throw new Error('sign-in answer does not match this window; sign in again');
  if (params.has('error')) throw new Error(`sign-in refused: ${params.get('error_description') ?? params.get('error')}`);
  const body = await tokenRequest(cfg, {
    grant_type: 'authorization_code', code: params.get('code'),
    redirect_uri: redirectUri(cfg), code_verifier: pending.verifier,
  });
  if (body.id_token && claims(body.id_token).nonce !== pending.nonce) throw new Error('sign-in answer has the wrong nonce');
  save(body);
  return true;
}

export const current = () => read(TOKENS);

let refreshing = null;
// A fresh access token, renewed a minute before it expires; when renewal fails (the session at
// the provider ended), back to the sign-in.
export async function accessToken(cfg) {
  const t = current();
  if (!t) return login(cfg);
  if (t.expiresAt - 60_000 > Date.now()) return t.access;
  refreshing ??= (async () => {
    try {
      if (!t.refresh) throw new Error('no refresh token');
      return save(await tokenRequest(cfg, { grant_type: 'refresh_token', refresh_token: t.refresh })).access;
    } catch (e) {
      console.warn(`token renewal failed (${e.message}); signing in again`);
      write(TOKENS, null);
      return login(cfg);
    } finally {
      refreshing = null;
    }
  })();
  return refreshing;
}

// Ends the session here and at the provider, then comes back to the sign-in.
export async function logout(cfg) {
  const t = current();
  write(TOKENS, null);
  const m = await discover(cfg);
  if (!m.end_session_endpoint) { location.assign(`${location.origin}/`); return; }
  const params = new URLSearchParams({ client_id: cfg.clientId, post_logout_redirect_uri: `${location.origin}/` });
  if (t?.id) params.set('id_token_hint', t.id);
  location.assign(`${m.end_session_endpoint}?${params}`);
}

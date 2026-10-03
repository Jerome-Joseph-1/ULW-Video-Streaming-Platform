// End-to-end encryption for the demo's encrypted room: the ONE file to replace when the OpenMLS
// browser client (feat/e2ee-wasm, clients/web-mls) is ready. chat.js uses only what this module
// exports below; nothing else in the page touches keys or ciphertext.
//
// THIS IS A STAND-IN, NOT MLS. It is a demo cipher built from WebCrypto: each member's device
// has an ECDH P-256 key, announced in the room; a message is encrypted with AES-256-GCM once
// per member, under a key derived (HKDF-SHA-256) from ECDH between the sender's key and that
// member's. What it does show truthfully is the platform's side of end-to-end encryption: chat
// stores, orders and relays opaque bytes and never holds a key (docs/integration/e2ee.md). It
// has none of MLS's properties: no forward secrecy or post-compromise security, no
// authentication of who announced a key, and the private key sits in localStorage.
//
// The contract (what an MLS module provides instead):
//   label                        what the page shows the cipher as
//   openSession({ user, room, members, publish })
//                                -> session; publish(payload) posts a handshake payload (a JSON
//                                   value) to the room, as MLS key packages, commits and
//                                   welcomes would be
//   session.receive({ sender, seq, payload })
//                                -> { text } a decrypted message, { note } a handshake event to
//                                   show, { error } one that cannot be read, or null; called for
//                                   every e2ee payload in the room, in seq order, history first
//   session.ready()              history is in: announce this device if it must
//   session.state()              -> { canSend, detail }
//   session.encrypt(text)        -> payload for chat (async)

export const label = 'demo cipher (ECDH P-256 + AES-GCM), not MLS';

const enc = new TextEncoder();
const dec = new TextDecoder();
const b64 = (buf) => btoa(String.fromCharCode(...new Uint8Array(buf)));
const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

async function deviceKey(user) {
  const name = `ulw-demo:${user}:e2ee-device-key`;
  try {
    const saved = JSON.parse(localStorage.getItem(name));
    if (saved) {
      return {
        privateKey: await crypto.subtle.importKey('jwk', saved.privateKey, { name: 'ECDH', namedCurve: 'P-256' }, false, ['deriveBits']),
        publicJwk: saved.publicJwk,
      };
    }
  } catch { /* make a new one */ }
  const pair = await crypto.subtle.generateKey({ name: 'ECDH', namedCurve: 'P-256' }, true, ['deriveBits']);
  const privateJwk = await crypto.subtle.exportKey('jwk', pair.privateKey);
  const { kty, crv, x, y } = await crypto.subtle.exportKey('jwk', pair.publicKey);
  const publicJwk = { kty, crv, x, y };
  try { localStorage.setItem(name, JSON.stringify({ privateKey: privateJwk, publicJwk })); } catch { /* ignore */ }
  return {
    privateKey: await crypto.subtle.importKey('jwk', privateJwk, { name: 'ECDH', namedCurve: 'P-256' }, false, ['deriveBits']),
    publicJwk,
  };
}

const sameKey = (a, b) => a && b && a.x === b.x && a.y === b.y;

export async function openSession({ user, room, members, publish }) {
  const me = await deviceKey(user);
  const keys = new Map(); // member -> public JWK, the latest each announced
  const aead = new Map(); // `${x}|${y}` of the other side -> AES key

  async function sharedKey(publicJwk) {
    const id = `${publicJwk.x}|${publicJwk.y}`;
    if (!aead.has(id)) {
      const peer = await crypto.subtle.importKey('jwk', { ...publicJwk, ext: true }, { name: 'ECDH', namedCurve: 'P-256' }, false, []);
      const bits = await crypto.subtle.deriveBits({ name: 'ECDH', public: peer }, me.privateKey, 256);
      const hkdf = await crypto.subtle.importKey('raw', bits, 'HKDF', false, ['deriveKey']);
      aead.set(id, await crypto.subtle.deriveKey(
        { name: 'HKDF', hash: 'SHA-256', salt: enc.encode(room), info: enc.encode('ulw-demo e2ee v1') },
        hkdf, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']));
    }
    return aead.get(id);
  }

  return {
    async receive({ sender, payload }) {
      if (!members.includes(sender)) return { error: `${sender} is not a member` };
      if (payload.k === 'key') {
        const fresh = !sameKey(keys.get(sender), payload.pub);
        keys.set(sender, payload.pub);
        return fresh ? { note: `${sender} announced a device key` } : null;
      }
      if (payload.k === 'msg') {
        const mine = payload.to?.[user];
        if (!mine) return { error: 'not encrypted for this device' };
        // ECDH(mine, sender's) is ECDH(sender's, mine); my own copy is ECDH(mine, mine).
        try {
          const key = await sharedKey(payload.from);
          const plain = await crypto.subtle.decrypt(
            { name: 'AES-GCM', iv: unb64(mine.iv), additionalData: enc.encode(`${room}|${sender}`) },
            key, unb64(mine.ct));
          return { text: dec.decode(plain) };
        } catch {
          return { error: 'cannot decrypt (the key it was sent to is gone)' };
        }
      }
      return null;
    },

    async ready() {
      if (!sameKey(keys.get(user), me.publicJwk)) {
        keys.set(user, me.publicJwk);
        await publish({ k: 'key', pub: me.publicJwk });
      }
    },

    state() {
      const missing = members.filter((m) => m !== user && !keys.has(m));
      return {
        canSend: missing.length < members.length - 1,
        detail: missing.length ? `waiting for ${missing.join(', ')} to open this room once (their device key)` : `encrypted to ${members.join(', ')}`,
      };
    },

    async encrypt(text) {
      const to = {};
      for (const [member, pub] of keys) {
        if (!members.includes(member)) continue;
        const iv = crypto.getRandomValues(new Uint8Array(12));
        // My own copy is under ECDH(me, me), so it is the sender's key either way.
        const ct = await crypto.subtle.encrypt(
          { name: 'AES-GCM', iv, additionalData: enc.encode(`${room}|${user}`) },
          await sharedKey(member === user ? me.publicJwk : pub), enc.encode(text));
        to[member] = { iv: b64(iv), ct: b64(ct) };
      }
      return { k: 'msg', from: me.publicJwk, to };
    },
  };
}

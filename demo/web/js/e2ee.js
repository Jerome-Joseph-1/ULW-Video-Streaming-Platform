// End-to-end encryption for the page's encrypted room. chat.js uses only openSession below.
//
// MLS (RFC 9420) through the OpenMLS browser client, clients/web-mls (ADR-0098),
// which the web proxy serves at /mls/ from this checkout. Every body in the room is one
// MLSMessage, as docs/integration/e2ee.md ("Browser client") lays out; MlsRoom (mls-room.js)
// does the group's bookkeeping. A build without that client gets the WebCrypto stand-in
// (e2ee-standin.js), which says on the page that it is not MLS.
//
// The contract:
//   openSession({ user, room, members, post(id, body) }) -> session
//     session.label           what the page shows the encryption as
//     session.receive(frame)  every `message` frame of the room, in seq order, history first;
//                             -> { text } decrypted, { note } a group event to show,
//                                { error } one that cannot be read, or null to show nothing
//     session.ready()         history is in: create the group, or ask to be added
//     session.state()         -> { canSend, complete, detail }
//     session.send(text)      encrypts and posts; -> the message id, whose echo is this
//                             device's own message (MLS cannot decrypt its own)
//     session.sendFailed(id, reason)   chat refused that send (an `error` with its id)
//     session.members()       [{identity, fingerprint}] of the group, when there is one
//   options.approve({identity, chatSender, fingerprintText}) -> Promise<boolean>: whether the
//   group's first member adds that device (MLS asks; the page shows an Approve/Deny card)
import { store } from './core.js';
import * as standin from './e2ee-standin.js';

let module = null;

async function loadMls() {
  try {
    const room = await import('/mls/mls-room.js');
    const wasm = await import('/mls/web_mls.js');
    await room.loadMls('/mls/web_mls_bg.wasm');
    return { ...room, ciphersuite: wasm.ciphersuite };
  } catch (e) {
    console.info(`no MLS client in this build (${e.message}); using the stand-in cipher`);
    return null;
  }
}

export async function openSession(options) {
  module ??= loadMls();
  const mls = await module;
  return mls ? openMls(mls, options) : standin.openSession(options);
}

const SUITES = { 1: 'MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519' };

async function openMls(mls, { user, room, members, post, approve, onEvent }) {
  // This browser profile's device for the user: its MLS identity is `<user>/<device>`, and its
  // state (keys, groups, what it still has to post) is kept in IndexedDB under that name.
  let device = store.get('mls-device', null);
  if (!device) {
    device = `${user}/${crypto.randomUUID().slice(0, 8)}`;
    store.set('mls-device', device);
  }
  let saved;
  try {
    saved = await mls.loadState(device);
  } catch (e) {
    const detail = e.message === 'device_in_use'
      ? `this browser's MLS device for ${user} (${device}) is open in another tab or window; close that one and reload`
      : `this browser's MLS device could not be read (${e.message})`;
    return blocked(detail);
  }
  const client = saved ? mls.MlsClient.importState(saved) : new mls.MlsClient(mls.utf8(device));
  let out = null;
  // Each key package's frame, and each user's newest: a device that asked again (a reload
  // without its state, an earlier run's device) supersedes what it asked before.
  const packages = new Map(); // keyPackageRef -> { seq, sender }
  const newest = new Map(); // chat user -> seq of their newest key package
  const pending = new Map(); // chat user -> { seq, abort } of the question on screen
  const suite = mls.ciphersuite();
  const fp = (f) => mls.formatFingerprint(f);

  const mlsRoom = new mls.MlsRoom({
    client,
    room,
    user,
    send: post,
    // The group's first member is asked about every device that wants in, with its fingerprint
    // to compare with what that device's page shows. No by default.
    approveKeyPackage: async (ask) => {
      const asked = packages.get(ask.keyPackageRef);
      if (asked && newest.get(asked.sender) > asked.seq) return false; // superseded
      if (ask.chatSender === user) return false; // this user's earlier device: one device per user here
      const controller = new AbortController();
      pending.set(ask.chatSender, { seq: asked?.seq ?? 0, abort: () => controller.abort() });
      try {
        return await approve({ ...ask, fingerprintText: fp(ask.fingerprint) }, controller.signal);
      } finally {
        if (pending.get(ask.chatSender)?.seq === (asked?.seq ?? 0)) pending.delete(ask.chatSender);
      }
    },
    onMessage: (m) => { out = { text: m.text, sender: m.sender }; },
    onEvent: (e) => {
      const who = mls.userPart(e.sender ?? e.who ?? e.identity ?? '');
      const note = {
        created: 'this device started the MLS group',
        joined: `this device joined the MLS group (epoch ${e.epoch})`,
        adding: `adding ${who}'s device ${e.who} (fingerprint ${e.fingerprint ? fp(e.fingerprint) : '?'})`,
        added: `the group now has ${e.members?.length} devices (epoch ${e.epoch})`,
        commit: `${who} changed the group (epoch ${e.epoch})`,
        denied: `${e.chatSender ?? who}'s device ${e.identity ?? ''} was not added: ${e.reason}`,
        lost: `a change to the group lost its epoch (${e.epoch}) and is retried`,
        removed: 'this device was removed from the group',
        error: `MLS: ${e.reason}`,
      }[e.type];
      if (note && !out) out = { note };
      onEvent?.(e);
    },
    // Awaited before anything is posted; a failure is an `error` event and posts nothing.
    onState: (state) => mls.saveState(device, state),
  });

  return {
    label: `MLS (RFC 9420), ciphersuite ${suite}${SUITES[suite] ? ` (${SUITES[suite]})` : ''}, OpenMLS in WebAssembly`,
    device,
    fingerprint: fp(client.fingerprint),

    async receive(frame) {
      try {
        const info = mls.inspect(mls.fromBase64url(frame.body));
        if (info.wireFormat === 'key_package') {
          packages.set(info.keyPackageRef, { seq: frame.seq, sender: frame.sender });
          newest.set(frame.sender, Math.max(newest.get(frame.sender) ?? 0, frame.seq));
          // A question about this user's older device is moot now: withdraw it (a no).
          const open = pending.get(frame.sender);
          if (open && open.seq < frame.seq) open.abort();
        }
      } catch {
        return null; // not an MLS message (the stand-in's, from before this build)
      }
      out = null;
      const done = mlsRoom.receive(frame); // its callbacks run before it returns; never throws
      const result = out;
      out = null;
      await done;
      if (result?.text !== undefined) {
        // The credential names the device; chat names the account that sent the frame.
        const who = mls.userPart(result.sender);
        return who === frame.sender ? { text: result.text } : { text: result.text, warning: `MLS sender ${result.sender} is not ${frame.sender}` };
      }
      return result;
    },

    async ready() {
      // What this device had not got back from the room yet, posted again under the same ids.
      await mlsRoom.resume();
      if (mlsRoom.joined) return;
      // The room's first member starts the group whenever its device is in none (after a demo
      // restarted without --wipe, an earlier group's devices are gone), and is then asked about
      // the newest device of each member who asked; everyone else asks to be added, once.
      if (members[0] === user) {
        await mlsRoom.create();
      } else if (!mlsRoom.memo?.announced) {
        await mlsRoom.announce();
      }
    },

    sendFailed: (id, reason) => mlsRoom.sendFailed(id, reason),

    members: () => mlsRoom.members().map((m) => ({ identity: m.identity, fingerprint: fp(m.fingerprint) })),

    state() {
      if (!mlsRoom.joined) {
        return { canSend: false, complete: false, detail: members[0] === user ? 'starting the MLS group' : `waiting for ${members[0]} to approve this device (they must have this room open)` };
      }
      const count = mlsRoom.members().length;
      return {
        canSend: true,
        complete: count >= members.length,
        detail: `MLS epoch ${mlsRoom.group.epoch}, encrypted to ${count} device${count === 1 ? '' : 's'}${count < members.length ? `; waiting for ${members.length - count} more` : ''}`,
      };
    },

    async send(text) {
      return mlsRoom.sendText(text);
    },
  };
}

// A session that can do nothing, and says why.
function blocked(detail) {
  return {
    label: 'MLS (unavailable here)',
    receive: async () => null,
    ready: async () => {},
    sendFailed: async () => {},
    members: () => [],
    state: () => ({ canSend: false, complete: false, detail }),
    send: async () => { throw new Error(detail); },
  };
}

// Forgets this browser's MLS device for the user (a new one is made on the next load): the way
// out when the group in the room was made by devices that are gone.
export function resetDevice() {
  store.set('mls-device', null);
}

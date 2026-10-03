// End-to-end encryption for the page's encrypted room. chat.js uses only openSession below.
//
// MLS (RFC 9420) through the OpenMLS browser client, clients/web-mls (feat/e2ee-wasm, ADR-0099),
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

async function openMls(mls, { user, room, members, post }) {
  // This browser profile's device for the user: its MLS identity is `<user>/<device>`, and its
  // state (keys and groups) is kept in IndexedDB under that name.
  let device = store.get('mls-device', null);
  if (!device) {
    device = `${user}/${crypto.randomUUID().slice(0, 8)}`;
    store.set('mls-device', device);
  }
  const saved = await mls.loadState(device).catch(() => null);
  const client = saved ? mls.MlsClient.importState(saved) : new mls.MlsClient(mls.utf8(device));
  let saving = Promise.resolve();
  let out = null;
  let lastId = null;
  let lastGroupSeq = 0;
  const keyPackages = [];
  const suite = mls.ciphersuite();

  const mlsRoom = new mls.MlsRoom({
    client,
    room,
    send: (id, body) => { lastId = id; post(id, body); },
    onMessage: (m) => { out = { text: m.text, sender: m.sender }; },
    onEvent: (e) => {
      const who = (e.sender ?? '').split('/')[0];
      const note = {
        created: 'this device started the MLS group',
        joined: `this device joined the MLS group (epoch ${e.epoch})`,
        adding: `adding ${String(e.who ?? '').split('/')[0]}'s device to the group`,
        added: `the group now has ${mlsRoom.group?.memberCount} devices (epoch ${e.epoch})`,
        commit: `${who} changed the group (epoch ${e.epoch})`,
      }[e.type];
      if (note && !out) out = { note };
    },
    onState: (state) => { saving = saving.then(() => mls.saveState(device, state)).catch(() => {}); },
  });

  return {
    label: `MLS (RFC 9420), ciphersuite ${suite}${SUITES[suite] ? ` (${SUITES[suite]})` : ''}, OpenMLS in WebAssembly`,

    async receive(frame) {
      let info;
      try {
        info = mls.inspect(mls.fromBase64url(frame.body));
      } catch {
        return null; // not an MLS message (the stand-in's, from before this build)
      }
      if (info.wireFormat === 'key_package') keyPackages.push(frame);
      else lastGroupSeq = frame.seq;
      out = null;
      mlsRoom.receive(frame);
      const result = out;
      out = null;
      if (result?.text !== undefined) {
        // The MLS credential names the device; chat names the account that sent the frame.
        const who = result.sender.split('/')[0];
        return who === frame.sender ? { text: result.text } : { text: result.text, warning: `MLS sender ${result.sender} is not ${frame.sender}` };
      }
      return result;
    },

    async ready() {
      if (mlsRoom.joined) return;
      // The room's first member starts the group whenever its device is not in one, and adds
      // whoever asked since the group traffic before it (an earlier group's, whose devices are
      // gone: a demo restarted without --wipe); everyone else asks to be added.
      if (members[0] === user) {
        mlsRoom.create();
        for (const frame of keyPackages) if (frame.sender !== user && frame.seq > lastGroupSeq) mlsRoom.receive(frame);
      } else {
        mlsRoom.announce();
      }
    },

    state() {
      if (!mlsRoom.joined) {
        return { canSend: false, complete: false, detail: `waiting to be added to the MLS group${members[0] === user ? '' : ` (by ${members[0]}, who must open this room)`}` };
      }
      const count = mlsRoom.group.memberCount;
      return {
        canSend: true,
        complete: count >= members.length,
        detail: `MLS epoch ${mlsRoom.group.epoch}, encrypted to ${count} device${count === 1 ? '' : 's'}${count < members.length ? `; waiting for ${members.length - count} more to open this room` : ''}`,
      };
    },

    async send(text) {
      lastId = null;
      mlsRoom.sendText(text);
      return lastId;
    },
  };
}

// Forgets this browser's MLS device for the user (a new one is made on the next load): the way
// out when the group in the room was made by devices that are gone.
export function resetDevice() {
  store.set('mls-device', null);
}

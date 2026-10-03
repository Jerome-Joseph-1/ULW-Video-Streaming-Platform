// An MLS group over one chat room, for the browser (and Node): the room-message convention of
// docs/integration/e2ee.md ("Browser client") on top of the WebAssembly client in web_mls.js.
//
// Every chat body in an encrypted room is one MLSMessage (RFC 9420, section 6), base64url as the
// chat protocol carries any body. Its wire format says what it is:
//   key_package      a device asking to be added; the member at the group's first leaf adds it
//   private_message  a commit or an application message for the group (OpenMLS's default
//                    policy, which the FFI bridge keeps, encrypts handshake messages too; the
//                    clear header names the group, the epoch and which it is)
//   welcome          the invitation that follows the commit that added someone
// A public_message (neither client sends one) is handled like a private one.
// The group id is the room id's text. The room's seq orders commits: for each epoch the first
// commit in seq order wins, and a member merges its own only when its echo arrives with no
// other commit for that epoch before it; it then sends the welcome.
//
// The transport is the caller's: `send(id, body)` posts a `send` command with that id and
// base64url body, and every `message` frame of the room goes to `receive(frame)`, the caller's
// own included and in seq order.

import init, { MlsClient, inspect } from "./web_mls.js";

let ready = null;

/** Loads the WebAssembly module once; `source` is its URL, Response or bytes (default: beside
 *  this file). */
export function loadMls(source) {
    ready ??= init(source === undefined ? undefined : { module_or_path: source });
    return ready;
}

export { MlsClient, inspect };

const encoder = new TextEncoder();
const decoder = new TextDecoder();

export function utf8(text) {
    return encoder.encode(text);
}

export function text(bytes) {
    return decoder.decode(bytes);
}

/** base64url without padding, as the chat protocol's `body`. */
export function toBase64url(bytes) {
    let binary = "";
    for (let i = 0; i < bytes.length; i += 0x8000) {
        binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    }
    return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export function fromBase64url(body) {
    const b64 = body.replace(/-/g, "+").replace(/_/g, "/");
    const binary = atob(b64 + "=".repeat((4 - (b64.length % 4)) % 4));
    const out = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) {
        out[i] = binary.charCodeAt(i);
    }
    return out;
}

function sameBytes(a, b) {
    return a.length === b.length && a.every((x, i) => x === b[i]);
}

function messageId() {
    return crypto.randomUUID();
}

export class MlsRoom {
    /**
     * @param {object} o
     * @param {MlsClient} o.client      this device
     * @param {string} o.room           the room id (canonical lowercase UUID): the group id
     * @param {(id: string, body: string) => void} o.send   posts one body to the room
     * @param {(m: {seq: number, sender: string, text: string}) => void} [o.onMessage]
     * @param {(e: {type: string, [k: string]: any}) => void} [o.onEvent]  joined, added,
     *        commit, ignored, error: for display
     * @param {(state: Uint8Array) => void} [o.onState]  the client's state after each change,
     *        for IndexedDB
     */
    constructor({ client, room, send, onMessage, onEvent, onState }) {
        this.client = client;
        this.room = room;
        this.groupId = utf8(room);
        this.post = send;
        this.onMessage = onMessage ?? (() => {});
        this.onEvent = onEvent ?? (() => {});
        this.onState = onState ?? (() => {});
        this.group = null;
        this.sent = new Map(); // id -> what it was: {kind, welcome?, epoch?}
        this.early = []; // group messages from before this device joined
        try {
            this.group = client.loadGroup(this.groupId);
        } catch {
            // not in it yet
        }
    }

    get joined() {
        return this.group !== null;
    }

    #save() {
        this.onState(this.client.exportState());
    }

    #send(kind, bytes, extra = {}) {
        const id = messageId();
        this.sent.set(id, { kind, ...extra });
        this.post(id, toBase64url(bytes));
        return id;
    }

    /** Starts the group, with this device its only member. */
    create() {
        if (!this.group) {
            this.group = this.client.createGroup(this.groupId);
            this.#save();
            this.onEvent({ type: "created", epoch: this.group.epoch });
        }
    }

    /** Posts a fresh key package, asking the group's members to add this device. */
    announce() {
        const kp = this.client.keyPackage();
        this.#save();
        this.#send("key_package", kp);
    }

    /** Encrypts and posts a text message. */
    sendText(message) {
        if (!this.group) {
            throw new Error("not in the group yet");
        }
        const sealed = this.group.encrypt(utf8(message));
        this.#save();
        this.#send("application", sealed);
    }

    /** Hands over one `message` frame of the room ({seq, sender, id, body}). */
    receive(frame) {
        const own = this.sent.get(frame.id);
        if (own) {
            this.sent.delete(frame.id);
            this.#ownEcho(own, frame);
            return;
        }
        let bytes;
        let info;
        try {
            bytes = fromBase64url(frame.body);
            info = inspect(bytes);
        } catch (e) {
            this.onEvent({ type: "ignored", seq: frame.seq, reason: e.message });
            return;
        }
        switch (info.wireFormat) {
            case "key_package":
                this.#keyPackage(bytes, info, frame);
                break;
            case "welcome":
                this.#welcome(bytes, frame);
                break;
            case "private_message":
            case "public_message":
                this.#groupMessage(bytes, info, frame);
                break;
            default:
                this.onEvent({ type: "ignored", seq: frame.seq, reason: info.wireFormat });
        }
    }

    #ownEcho(own, frame) {
        if (own.kind !== "commit" || !this.group) {
            return;
        }
        // Every lower seq has been handled: if no other commit took the epoch, this one won.
        if (this.group.hasPendingCommit && this.group.epoch === own.epoch) {
            this.group.mergePendingCommit();
            this.#save();
            this.onEvent({ type: "added", seq: frame.seq, epoch: this.group.epoch });
            this.#send("welcome", own.welcome);
        } else if (this.group.hasPendingCommit) {
            this.group.clearPendingCommit();
            this.#save();
        }
    }

    #keyPackage(bytes, info, frame) {
        if (!this.group || this.group.hasPendingCommit) {
            return;
        }
        // One member adds: the one at the first leaf, so two never race for the same epoch.
        const members = this.group.members();
        if (!sameBytes(members[0], this.client.identity)) {
            return;
        }
        if (members.some((m) => sameBytes(m, info.identity))) {
            return;
        }
        const epoch = this.group.epoch;
        const added = this.group.add([bytes]);
        this.#save();
        this.#send("commit", added.commit, { welcome: added.welcome, epoch });
        this.onEvent({ type: "adding", seq: frame.seq, who: text(info.identity) });
    }

    #welcome(bytes, frame) {
        if (this.group) {
            return;
        }
        try {
            this.group = this.client.joinGroup(bytes);
        } catch (e) {
            // For another device.
            return;
        }
        this.#save();
        this.onEvent({ type: "joined", seq: frame.seq, epoch: this.group.epoch });
        const early = this.early;
        this.early = [];
        for (const m of early) {
            this.#groupMessage(m.bytes, m.info, m.frame);
        }
    }

    #groupMessage(bytes, info, frame) {
        if (!sameBytes(info.groupId, this.groupId)) {
            this.onEvent({ type: "ignored", seq: frame.seq, reason: "another group" });
            return;
        }
        if (!this.group) {
            this.early.push({ bytes, info, frame });
            return;
        }
        if (info.epoch !== this.group.epoch) {
            // An older epoch's message, or a commit that lost its epoch to an earlier one.
            this.onEvent({ type: "ignored", seq: frame.seq, reason: `epoch ${info.epoch}` });
            return;
        }
        let r;
        try {
            r = this.group.process(bytes);
        } catch (e) {
            this.onEvent({ type: "error", seq: frame.seq, reason: e.message });
            return;
        }
        this.#save();
        if (r.kind === "application") {
            this.onMessage({ seq: frame.seq, sender: text(r.sender), text: text(r.plaintext) });
        } else {
            this.onEvent({ type: r.kind, seq: frame.seq, sender: text(r.sender), epoch: this.group.epoch });
        }
    }
}

// The client's state in IndexedDB: one record per device name.
const DB = "ulw-mls";
const STORE = "devices";

function openDb() {
    return new Promise((resolve, reject) => {
        const req = indexedDB.open(DB, 1);
        req.onupgradeneeded = () => req.result.createObjectStore(STORE);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
    });
}

export async function saveState(name, state) {
    const db = await openDb();
    await new Promise((resolve, reject) => {
        const tx = db.transaction(STORE, "readwrite");
        tx.objectStore(STORE).put(state, name);
        tx.oncomplete = resolve;
        tx.onerror = () => reject(tx.error);
    });
    db.close();
}

export async function loadState(name) {
    const db = await openDb();
    const state = await new Promise((resolve, reject) => {
        const req = db.transaction(STORE).objectStore(STORE).get(name);
        req.onsuccess = () => resolve(req.result ?? null);
        req.onerror = () => reject(req.error);
    });
    db.close();
    return state;
}

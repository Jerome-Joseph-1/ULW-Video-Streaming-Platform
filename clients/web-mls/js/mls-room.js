// An MLS group over one chat room, for the browser (and Node): the room-message convention of
// docs/integration/e2ee.md ("Browser client") on top of the WebAssembly client in web_mls.js.
//
// Every chat body in an encrypted room is one MLSMessage (RFC 9420, section 6), base64url as the
// chat protocol carries any body. Its wire format says what it is:
//   key_package      a device asking to be added. The member at the group's first leaf adds it
//                    once `approveKeyPackage` says yes (no by default); its credential's user
//                    part must be the chat user who posted it
//   private_message  a commit or an application message for the group (OpenMLS's default
//                    policy, which the FFI bridge keeps, encrypts handshake messages too; the
//                    clear header names the group, the epoch and which it is)
//   welcome          the invitation that follows the commit that added someone; taken only by a
//                    device that announced itself, and only into this room's group
// A public_message (neither client sends one) is handled like a private one; proposals are
// dropped. The group id is the room id's text. The room's seq orders commits: for each epoch
// the first commit in seq order wins, a commit adding members counts only from the first
// leaf, and a member merges its own only when its echo arrives with no other commit for that
// epoch before it; it then sends the welcome.
//
// Everything this device posts goes through an outbox kept in the device's state: the state is
// saved (`onState`, awaited) before the body is posted, and an entry leaves the outbox when its
// echo arrives. `resume()` posts what is still outstanding, under the same ids, so a reload or
// a failed send never loses a commit; chat sequences a resent id once.
//
// The transport is the caller's: `send(id, body)` posts a `send` command with that id and
// base64url body, every `message` frame of the room goes to `receive(frame)` (history first,
// in seq order, the caller's own included), and a send the server refused goes to
// `sendFailed(id, reason)`.

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
    return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/={1,2}$/, "");
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

/** The chat user a device identity names: the part before the first `/` (`alice/laptop` is
 *  alice's), or the whole identity. */
export function userPart(identity) {
    const name = typeof identity === "string" ? identity : text(identity);
    const slash = name.indexOf("/");
    return slash < 0 ? name : name.slice(0, slash);
}

/** A fingerprint as people compare it: groups of four hex digits. */
export function formatFingerprint(fp) {
    return fp.match(/.{1,4}/g).join(" ");
}

function sameBytes(a, b) {
    return a.length === b.length && a.every((x, i) => x === b[i]);
}

const quiet = (p) => p.catch(() => {});

// Past epochs whose application messages still decrypt: the client keeps this many
// (web_mls core::MAX_PAST_EPOCHS).
const PAST_EPOCHS = 4;
// Group messages kept while waiting for a welcome.
const EARLY_LIMIT = 256;
// Key packages decided on, remembered so a replayed history does not ask again.
const DECIDED_LIMIT = 1024;
// Refusals that resending under the same id cannot fix.
const FINAL_REFUSALS = new Set(["conflict", "malformed", "bad_body", "bad_room", "bad_id", "not_member", "too_large"]);

export class MlsRoom {
    /**
     * @param {object} o
     * @param {MlsClient} o.client      this device
     * @param {string} o.room           the room id (a UUID); the group id is its lowercase text
     * @param {string} [o.user]         this device's chat user id (default: its identity's user part)
     * @param {(id: string, body: string) => void} o.send   posts one body to the room
     * @param {(m: {seq, sender, text}) => void} [o.onMessage]  a decrypted message
     * @param {(e: {type: string}) => void} [o.onEvent]  created, adding, added, lost, joined,
     *        commit, removed, denied, ignored, error: for display
     * @param {(state: Uint8Array) => void|Promise<void>} [o.onState]  the client's state, to
     *        keep (IndexedDB); awaited before anything is posted, and a rejection is an error
     * @param {(kp: {identity, chatSender, fingerprint, keyPackageRef}) => boolean|Promise<boolean>}
     *        [o.approveKeyPackage]  whether to add the device that posted this key package;
     *        no by default. Asked only on the device at the group's first leaf, and only when
     *        the identity's user part is the chat user who posted it.
     */
    constructor({ client, room, user, send, onMessage, onEvent, onState, approveKeyPackage }) {
        this.client = client;
        this.room = room.toLowerCase();
        this.groupId = utf8(this.room);
        this.user = user ?? userPart(client.identity);
        this.post = send;
        this.onMessage = onMessage ?? (() => {});
        this.onEvent = onEvent ?? (() => {});
        this.onState = onState ?? (() => {});
        this.approve = approveKeyPackage ?? (() => false);
        this.group = null;
        this.early = []; // group messages from before this device joined
        this.queue = []; // key packages waiting to be decided on
        this.draining = false;
        this.chain = Promise.resolve(); // saves and posts, in order
        this.memo = this.#loadMemo();
        try {
            this.group = client.loadGroup(this.groupId);
        } catch {
            // not in it yet
        }
    }

    get joined() {
        return this.group !== null && this.group.active;
    }

    /** The group's members, `[{identity, fingerprint, leaf}]`; none before joining. */
    members() {
        if (!this.group) {
            return [];
        }
        return this.group.members().map((m) => ({ identity: text(m.identity), fingerprint: m.fingerprint, leaf: m.leaf }));
    }

    // --- what is kept with the state --------------------------------------------------------

    #allMemos() {
        try {
            const data = this.client.appData;
            return data.length ? JSON.parse(text(data)) : {};
        } catch {
            return {};
        }
    }

    #loadMemo() {
        const memo = this.#allMemos()[this.room] ?? {};
        return { outbox: memo.outbox ?? [], announced: memo.announced ?? false, decided: memo.decided ?? [] };
    }

    // Saves the state as it is now, then posts `entries`. Resolves when both are done. A save
    // that fails posts nothing (the entries stay in the outbox for resume()) and rejects.
    #commit(entries = []) {
        const all = this.#allMemos();
        this.memo.decided = this.memo.decided.slice(-DECIDED_LIMIT);
        all[this.room] = this.memo;
        this.client.appData = utf8(JSON.stringify(all));
        const state = this.client.exportState();
        const step = this.chain.then(async () => {
            try {
                await this.onState(state);
            } catch (e) {
                this.onEvent({ type: "error", reason: `state not saved: ${e?.message ?? e}` });
                throw e;
            }
            for (const entry of entries) {
                this.post(entry.id, entry.body);
            }
        });
        this.chain = quiet(step);
        return step;
    }

    #outbox(kind, bytes, extra = {}) {
        const entry = { id: crypto.randomUUID(), kind, body: toBase64url(bytes), ...extra };
        this.memo.outbox.push(entry);
        return entry;
    }

    // --- what the page calls ------------------------------------------------------------------

    /** Starts the group, with this device its only member. */
    async create() {
        if (this.group) {
            return;
        }
        this.group = this.client.createGroup(this.groupId);
        const saved = this.#commit();
        this.onEvent({ type: "created", epoch: this.group.epoch, fingerprint: this.client.fingerprint });
        await saved;
        void this.#drain();
    }

    /** Posts a fresh key package, asking the group's first member to add this device. */
    async announce() {
        const kp = this.client.keyPackage();
        this.memo.announced = true;
        await this.#commit([this.#outbox("key_package", kp)]);
    }

    /** Encrypts and posts a text message; resolves to its id once saved and posted. */
    async sendText(message) {
        if (!this.joined) {
            throw new Error("not in the group");
        }
        const entry = this.#outbox("application", this.group.encrypt(utf8(message)));
        await this.#commit([entry]);
        return entry.id;
    }

    /** Posts again, under the same ids, whatever has not come back from the room: after a
     *  reload once history is in, and after a reconnect. */
    async resume() {
        await this.#commit(this.memo.outbox);
    }

    /** The server refused the send `id` with `reason` (its `error`). A retryable refusal posts
     *  it again under the same id (wait `retry_after_ms` first); a final one drops it. */
    async sendFailed(id, reason) {
        const entry = this.memo.outbox.find((e) => e.id === id);
        if (!entry) {
            return;
        }
        if (!FINAL_REFUSALS.has(reason)) {
            await this.#commit([entry]);
            return;
        }
        this.memo.outbox = this.memo.outbox.filter((e) => e !== entry);
        this.onEvent({ type: "error", reason: `${entry.kind} refused: ${reason}` });
        if (entry.kind === "commit" && this.group?.hasPendingCommit) {
            this.group.clearPendingCommit();
        }
        await this.#commit();
        void this.#drain();
    }

    /** Hands over one `message` frame of the room ({seq, sender, id, body}). Callbacks for it
     *  run before this returns; the promise settles once what it caused is saved and posted.
     *  Nothing it does throws: failures are `error` events. */
    receive(frame) {
        try {
            return quiet(this.#receive(frame));
        } catch (e) {
            this.onEvent({ type: "error", seq: frame.seq, reason: e?.message ?? String(e) });
            return Promise.resolve();
        }
    }

    // --- incoming -----------------------------------------------------------------------------

    #receive(frame) {
        const own = frame.sender === this.user ? this.memo.outbox.find((e) => e.id === frame.id) : undefined;
        if (own) {
            return this.#ownEcho(own, frame);
        }
        let bytes;
        let info;
        try {
            bytes = fromBase64url(frame.body);
            info = inspect(bytes);
        } catch (e) {
            this.onEvent({ type: "ignored", seq: frame.seq, reason: e.message });
            return Promise.resolve();
        }
        switch (info.wireFormat) {
            case "key_package":
                this.queue.push({ bytes, info, frame });
                void this.#drain();
                return Promise.resolve();
            case "welcome":
                return this.#welcome(bytes, frame);
            case "private_message":
            case "public_message":
                return this.#groupMessage(bytes, info, frame);
            default:
                this.onEvent({ type: "ignored", seq: frame.seq, reason: info.wireFormat });
                return Promise.resolve();
        }
    }

    #ownEcho(entry, frame) {
        this.memo.outbox = this.memo.outbox.filter((e) => e !== entry);
        if (entry.kind !== "commit") {
            return this.#commit();
        }
        // Every lower seq has been handled: if no other commit took the epoch, this one won.
        if (this.group?.hasPendingCommit && this.group.epoch === entry.epoch) {
            this.group.mergePendingCommit();
            const welcome = this.#outbox("welcome", fromBase64url(entry.welcome));
            this.onEvent({ type: "added", seq: frame.seq, epoch: this.group.epoch, members: this.members() });
            const done = this.#commit([welcome]);
            void this.#drain();
            return done;
        }
        if (this.group?.hasPendingCommit) {
            this.group.clearPendingCommit();
        }
        // Lost the epoch: the devices it added are decided on again.
        this.memo.decided = this.memo.decided.filter((r) => !(entry.refs ?? []).includes(r));
        for (const kp of entry.packages ?? []) {
            const bytes = fromBase64url(kp.body);
            this.queue.push({ bytes, info: inspect(bytes), frame: kp.frame });
        }
        this.onEvent({ type: "lost", seq: frame.seq, epoch: entry.epoch });
        const done = this.#commit();
        void this.#drain();
        return done;
    }

    #canAdd() {
        if (!this.joined || this.group.hasPendingCommit || this.memo.outbox.some((e) => e.kind === "commit")) {
            return false;
        }
        return sameBytes(this.group.members()[0].identity, this.client.identity);
    }

    // Decides on queued key packages one at a time, while this device may add. Never rejects (a
    // failure is already an error event), so callers start it with `void` and go on.
    async #drain() {
        if (this.draining) {
            return;
        }
        this.draining = true;
        try {
            while (this.queue.length && this.#canAdd()) {
                const { bytes, info, frame } = this.queue.shift();
                const ref = info.keyPackageRef;
                const identity = text(info.identity);
                if (this.memo.decided.includes(ref) || this.members().some((m) => m.identity === identity)) {
                    continue;
                }
                const ask = { identity, chatSender: frame.sender, fingerprint: info.fingerprint, keyPackageRef: ref };
                let yes = false;
                if (userPart(identity) !== frame.sender) {
                    this.onEvent({ type: "denied", seq: frame.seq, ...ask, reason: "credential names another user" });
                } else {
                    try {
                        yes = (await this.approve(ask)) === true;
                    } catch (e) {
                        this.onEvent({ type: "error", seq: frame.seq, reason: `approval failed: ${e?.message ?? e}` });
                    }
                    if (!yes) {
                        this.onEvent({ type: "denied", seq: frame.seq, ...ask, reason: "not approved" });
                    }
                }
                if (yes && !this.#canAdd()) {
                    // The group moved on while the approval was pending: decide again later.
                    this.queue.unshift({ bytes, info, frame });
                    break;
                }
                this.memo.decided.push(ref);
                if (!yes) {
                    await this.#commit();
                    continue;
                }
                const epoch = this.group.epoch;
                const added = this.group.add([bytes]);
                const entry = this.#outbox("commit", added.commit, {
                    epoch,
                    welcome: toBase64url(added.welcome),
                    refs: [ref],
                    packages: [{ body: toBase64url(bytes), frame: { seq: frame.seq, sender: frame.sender } }],
                });
                this.onEvent({ type: "adding", seq: frame.seq, who: identity, fingerprint: info.fingerprint });
                await this.#commit([entry]);
            }
        } catch {
            // Already an error event.
        } finally {
            this.draining = false;
        }
    }

    #welcome(bytes, frame) {
        if (this.group || !this.memo.announced) {
            return Promise.resolve();
        }
        try {
            this.group = this.client.joinGroup(bytes, this.groupId);
        } catch {
            // For another device or another group: nothing is kept.
            return Promise.resolve();
        }
        this.memo.announced = false;
        // The welcome used the key package; it is no longer outstanding.
        this.memo.outbox = this.memo.outbox.filter((e) => e.kind !== "key_package");
        this.onEvent({ type: "joined", seq: frame.seq, epoch: this.group.epoch, members: this.members() });
        const done = this.#commit();
        const early = this.early;
        this.early = [];
        for (const m of early) {
            quiet(this.#groupMessage(m.bytes, m.info, m.frame));
        }
        void this.#drain();
        return done;
    }

    #groupMessage(bytes, info, frame) {
        if (!sameBytes(info.groupId, this.groupId)) {
            this.onEvent({ type: "ignored", seq: frame.seq, reason: "another group" });
            return Promise.resolve();
        }
        if (info.contentType === "proposal") {
            this.onEvent({ type: "ignored", seq: frame.seq, reason: "proposal" });
            return Promise.resolve();
        }
        if (!this.group) {
            if (this.memo.announced) {
                if (this.early.length >= EARLY_LIMIT) {
                    this.early.shift();
                }
                this.early.push({ bytes, info, frame });
            }
            return Promise.resolve();
        }
        if (!this.group.active) {
            return Promise.resolve();
        }
        const epoch = this.group.epoch;
        const inWindow = info.contentType === "commit"
            ? info.epoch === epoch
            : info.epoch <= epoch && info.epoch >= epoch - PAST_EPOCHS;
        if (!inWindow) {
            // An old epoch's message, a commit that lost its epoch, or one from the future.
            this.onEvent({ type: "ignored", seq: frame.seq, reason: `epoch ${info.epoch}` });
            return Promise.resolve();
        }
        let r;
        try {
            r = this.group.process(bytes);
        } catch (e) {
            this.onEvent({ type: "error", seq: frame.seq, reason: e.message });
            return Promise.resolve();
        }
        const done = this.#commit();
        if (r.kind === "application") {
            this.onMessage({ seq: frame.seq, sender: text(r.sender), text: text(r.plaintext) });
        } else if (r.selfRemoved) {
            this.onEvent({ type: "removed", seq: frame.seq, sender: text(r.sender) });
        } else {
            this.onEvent({ type: r.kind, seq: frame.seq, sender: text(r.sender), epoch: this.group.epoch, members: this.members() });
            void this.#drain();
        }
        return done;
    }
}

// --- the device's state in IndexedDB, one tab per device -----------------------------------

const DB = "ulw-mls";
const STORE = "devices";
const held = new Set();

/** Takes this tab's exclusive hold on device `name` (Web Locks), kept until the page goes.
 *  Resolves false when another tab of this browser profile has it. */
export function lockDevice(name) {
    if (held.has(name)) {
        return Promise.resolve(true);
    }
    if (!globalThis.navigator?.locks) {
        held.add(name);
        return Promise.resolve(true);
    }
    return new Promise((resolve, reject) => {
        navigator.locks
            .request(`ulw-mls:${name}`, { ifAvailable: true }, (lock) => {
                if (!lock) {
                    resolve(false);
                    return undefined;
                }
                held.add(name);
                resolve(true);
                return new Promise(() => {}); // held for the page's life
            })
            .catch(reject);
    });
}

function openDb() {
    return new Promise((resolve, reject) => {
        const req = indexedDB.open(DB, 1);
        req.onupgradeneeded = () => req.result.createObjectStore(STORE);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
    });
}

/** Saves device `name`'s state. Throws `device_in_use` unless this tab holds the device
 *  (loadState or lockDevice took it). */
export async function saveState(name, state) {
    if (!held.has(name)) {
        throw new Error("device_in_use");
    }
    const db = await openDb();
    await new Promise((resolve, reject) => {
        const tx = db.transaction(STORE, "readwrite");
        tx.objectStore(STORE).put(state, name);
        tx.oncomplete = resolve;
        tx.onerror = () => reject(tx.error);
    });
    db.close();
}

/** Device `name`'s saved state, or null. Takes the device for this tab first, and throws
 *  `device_in_use` when another tab has it. */
export async function loadState(name) {
    if (!(await lockDevice(name))) {
        throw new Error("device_in_use");
    }
    const db = await openDb();
    const state = await new Promise((resolve, reject) => {
        const req = db.transaction(STORE).objectStore(STORE).get(name);
        req.onsuccess = () => resolve(req.result ?? null);
        req.onerror = () => reject(req.error);
    });
    db.close();
    return state;
}

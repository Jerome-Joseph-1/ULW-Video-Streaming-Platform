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
//
// With the chat server's key directory (ADR-0102), devices no longer post key packages to the
// room. Each device registers itself (`<user>/<device id>` is its credential's identity) and
// publishes single-use key packages and a last-resort one through an `MlsDirectory`; the
// device at the first leaf calls `reconcile(users)` with the room's member list, which lists
// every member's devices, claims a package of each device the group lacks, asks
// `approveKeyPackage`, and adds them all in one commit; it also removes the devices of users
// no longer listed and devices their users retired. Rooms still accept key packages posted the
// old way, so pages without a directory keep working.

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
// A device id as the directory knows it: a canonical lowercase UUID.
const DEVICE_ID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

/** Runs `save` for one state at a time, in the order asked: hand the same one to every MlsRoom
 *  and the MlsDirectory of a device, so an older state never lands after a newer one. */
export function inOrder(save) {
    let last = Promise.resolve();
    return (state) => {
        const next = last.then(() => save(state));
        last = next.catch(() => {});
        return next;
    };
}

// --- the key directory ------------------------------------------------------------------------

// Directory refusals that asking again, a little later, may cure.
const DIRECTORY_RETRY = new Set(["rate_limited", "busy", "unavailable"]);
const DIRECTORY_TRIES = 5;

/** The chat server's key directory (docs/integration/e2ee.md, "Key directory") for one device,
 *  over the chat socket the page already has. Every frame the socket receives goes to
 *  `receive(frame)`, which takes the directory's answers and `replenish` frames and says
 *  whether it took the frame. */
export class MlsDirectory {
    /**
     * @param {object} o
     * @param {MlsClient} o.client    this device; its identity is `<user>/<device>`
     * @param {string} o.device        this device's id: a lowercase UUID, minted once per device
     * @param {(command: object) => void} o.send  sends one command as JSON on the chat socket
     * @param {(state: Uint8Array) => void|Promise<void>} [o.onState]  saves the client's state;
     *        awaited before any package is published, since a package whose private key was not
     *        saved can never be used. Use the rooms' own saver, through `inOrder`
     * @param {number} [o.target]      how many single-use packages to keep published (20)
     * @param {(e: object) => void} [o.onEvent]  published, replenish, error
     * @param {number} [o.timeoutMs]   how long to wait for an answer (15 s)
     */
    constructor({ client, device, send, onState, target, onEvent, timeoutMs }) {
        if (!DEVICE_ID.test(device)) {
            throw new Error("invalid_argument");
        }
        this.client = client;
        this.device = device;
        this.post = send;
        this.onState = onState ?? (() => {});
        this.target = Math.min(Math.max(target ?? 20, 1), 100);
        this.onEvent = onEvent ?? (() => {});
        this.timeoutMs = timeoutMs ?? 15000;
        this.pending = new Map();
        this.stocking = null;
    }

    /** Takes a frame from the chat socket if it is the directory's: true when it was. */
    receive(frame) {
        if (frame.type === "replenish") {
            if (frame.device === this.device) {
                this.onEvent({ type: "replenish" });
                void this.ensureStock().catch((e) => this.onEvent({ type: "error", reason: e.message }));
            }
            return true;
        }
        const waiting = frame.id === undefined ? undefined : this.pending.get(frame.id);
        if (!waiting) {
            return false;
        }
        this.pending.delete(frame.id);
        clearTimeout(waiting.timer);
        if (frame.type !== "error") {
            waiting.resolve(frame);
            return true;
        }
        if (DIRECTORY_RETRY.has(frame.reason) && waiting.tries < DIRECTORY_TRIES) {
            setTimeout(() => this.#send(waiting), frame.retry_after_ms ?? 500 * waiting.tries);
            return true;
        }
        const error = new Error(frame.reason);
        error.frame = frame;
        waiting.reject(error);
        return true;
    }

    /** Sends one directory command and resolves with its answer; rejects with the refusal's
     *  reason as the message. Rate limits and outages are waited out a few times first. */
    request(command) {
        return new Promise((resolve, reject) => {
            this.#send({ command, resolve, reject, tries: 0 });
        });
    }

    #send(waiting) {
        const id = `dir-${crypto.randomUUID()}`;
        waiting.tries += 1;
        waiting.timer = setTimeout(() => {
            this.pending.delete(id);
            waiting.reject(new Error("timeout"));
        }, this.timeoutMs);
        this.pending.set(id, waiting);
        this.post({ ...waiting.command, id });
    }

    /** Registers this device (again: it is idempotent) and tops its packages up: on every
     *  connect, after a `replenish`, and after joining a group, which spent one. */
    ensureStock() {
        this.stocking ??= (async () => {
            try {
                const supply = await this.request({ type: "register_device", device: this.device });
                return await this.#topUp(supply);
            } finally {
                this.stocking = null;
            }
        })();
        return this.stocking;
    }

    async #topUp(supply) {
        const missing = Math.max(0, this.target - supply.key_packages);
        const needLastResort = supply.last_resort !== "fresh";
        // Topped up once half the supply is gone, or when the last resort was used.
        if (!needLastResort && missing * 2 < this.target) {
            return supply;
        }
        const command = { type: "publish_key_packages", device: this.device };
        if (missing > 0) {
            command.key_packages = Array.from({ length: missing }, () => toBase64url(this.client.keyPackage()));
        }
        if (needLastResort) {
            command.last_resort = toBase64url(this.client.lastResortKeyPackage());
        }
        // The private halves are in the state now; it is saved before anyone can be given them.
        await this.onState(this.client.exportState());
        const published = await this.request(command);
        this.onEvent({ type: "published", keyPackages: published.key_packages, lastResort: published.last_resort });
        return published;
    }

    /** The user's live devices: `[{device}]`, with `key_packages` and `last_resort` for the
     *  page's own user. Another user's only if the two share a direct or group chat. */
    async devices(user) {
        return (await this.request({ type: "devices", user })).devices;
    }

    /** One key package of each named device of `user` (every live one without `devices`):
     *  `{key_packages: [{device, key_package, last_resort}], exhausted, gone, unavailable}`. */
    claim(user, devices) {
        const command = { type: "claim_key_packages", user };
        if (devices?.length) {
            command.devices = devices;
        }
        return this.request(command);
    }

    /** Retires this device for good: its packages go and its id never comes back. */
    retire() {
        return this.request({ type: "retire_device", device: this.device });
    }
}

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
     * @param {(kp: {identity, chatSender, fingerprint, keyPackageRef, device?, lastResort?}) => boolean|Promise<boolean>}
     *        [o.approveKeyPackage]  whether to add the device that posted this key package, or
     *        whose package the directory handed out (then with `device`); no by default. Asked
     *        only on the device at the group's first leaf, and only when the identity's user
     *        part is the chat user who posted it, or, from the directory, when the identity is
     *        exactly `<user>/<device>` of the device claimed.
     * @param {MlsDirectory} [o.directory]  the key directory: `announce()` publishes through
     *        it instead of posting to the room, and `reconcile(users)` adds devices from it
     */
    constructor({ client, room, user, send, onMessage, onEvent, onState, approveKeyPackage, directory }) {
        this.client = client;
        this.room = room.toLowerCase();
        this.groupId = utf8(this.room);
        this.user = user ?? userPart(client.identity);
        this.post = send;
        this.onMessage = onMessage ?? (() => {});
        this.onEvent = onEvent ?? (() => {});
        this.onState = onState ?? (() => {});
        this.approve = approveKeyPackage ?? (() => false);
        this.directory = directory ?? null;
        this.users = null; // the member list reconcile() was last given
        this.reconciling = false;
        this.reconcileAgain = false;
        this.group = null;
        this.early = []; // group messages from before this device joined
        this.queue = []; // key packages waiting to be decided on
        this.draining = false;
        this.chain = null; // the last save and post, which the next one waits for
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
        return {
            outbox: memo.outbox ?? [],
            announced: memo.announced ?? false,
            decided: memo.decided ?? [],
            refused: memo.refused ?? [], // directory devices whose addition was not approved
        };
    }

    // Saves the state as it is now, then posts `entries`. Resolves when both are done. A save
    // that fails posts nothing (the entries stay in the outbox for resume()) and rejects.
    #commit(entries = []) {
        const all = this.#allMemos();
        this.memo.decided = this.memo.decided.slice(-DECIDED_LIMIT);
        this.memo.refused = this.memo.refused.slice(-DECIDED_LIMIT);
        all[this.room] = this.memo;
        this.client.appData = utf8(JSON.stringify(all));
        const state = this.client.exportState();
        const step = (this.chain ?? Promise.resolve()).then(async () => {
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

    /** Asks to be added: with a directory, registers this device and publishes its packages
     *  there, for the group's first member to find; without one, posts a fresh key package to
     *  the room. Either way this device then takes the room's welcome. */
    async announce() {
        if (this.directory) {
            await this.directory.ensureStock();
            this.memo.announced = true;
            await this.#commit();
            return;
        }
        const kp = this.client.keyPackage();
        this.memo.announced = true;
        await this.#commit([this.#outbox("key_package", kp)]);
    }

    /** Brings the group in line with the room's member list `users` (chat user ids), on the
     *  device at the first leaf; elsewhere it does nothing. Every live device of each user that
     *  the group lacks is claimed from the directory and, once approved, added in one commit;
     *  a device of a user not listed, or one its user retired, is removed, one commit at a
     *  time. Call it when the member list is read, on each `member` frame, and now and then: a
     *  device registered since is found the next time. Never rejects: failures are events. */
    async reconcile(users) {
        if (!this.directory) {
            throw new Error("no directory");
        }
        this.users = [...new Set(users)];
        if (this.reconciling) {
            this.reconcileAgain = true;
            return;
        }
        this.reconciling = true;
        try {
            do {
                this.reconcileAgain = false;
                await this.#reconcileOnce(this.users);
            } while (this.reconcileAgain);
        } catch (e) {
            this.onEvent({ type: "error", reason: `reconcile: ${e?.message ?? e}` });
        } finally {
            this.reconciling = false;
        }
    }

    async #reconcileOnce(users) {
        if (!this.#canAdd()) {
            return;
        }
        const live = new Map();
        for (const user of users) {
            live.set(user, new Set((await this.directory.devices(user)).map((d) => d.device)));
        }
        if (!this.#canAdd()) {
            return;
        }
        // Removals first, one commit each; the echo of each runs this again.
        const own = text(this.client.identity);
        for (const m of this.members()) {
            if (m.identity === own) {
                continue;
            }
            const user = userPart(m.identity);
            const device = m.identity.slice(user.length + 1);
            // A device of the old convention (any name but a device id) goes only with its user.
            const gone = !live.has(user) || (DEVICE_ID.test(device) && !live.get(user).has(device));
            if (gone) {
                const epoch = this.group.epoch;
                const commit = this.group.remove(utf8(m.identity));
                const entry = this.#outbox("commit", commit, { epoch, directory: true });
                this.onEvent({ type: "removing", who: m.identity });
                await this.#commit([entry]);
                return;
            }
        }
        const have = new Set(this.members().map((m) => m.identity));
        const approved = [];
        for (const [user, devices] of live) {
            const missing = [...devices].filter((d) => !have.has(`${user}/${d}`) && !this.memo.refused.includes(`${user}/${d}`));
            if (!missing.length) {
                continue;
            }
            const answer = await this.directory.claim(user, missing);
            for (const d of answer.exhausted ?? []) {
                this.onEvent({ type: "exhausted", who: `${user}/${d}` });
            }
            for (const kp of answer.key_packages ?? []) {
                const added = await this.#decide(user, kp);
                if (added) {
                    approved.push(added);
                }
            }
        }
        if (!approved.length || !this.#canAdd()) {
            // Packages claimed for a group that moved on meanwhile are spent; the next round
            // claims others.
            await this.#commit();
            if (approved.length) {
                this.reconcileAgain = true;
            }
            return;
        }
        const epoch = this.group.epoch;
        const added = this.group.add(approved.map((a) => a.bytes));
        const entry = this.#outbox("commit", added.commit, {
            epoch,
            welcome: toBase64url(added.welcome),
            refs: approved.map((a) => a.ref),
            directory: true,
        });
        for (const a of approved) {
            this.onEvent({ type: "adding", who: a.identity, fingerprint: a.fingerprint, lastResort: a.lastResort });
        }
        await this.#commit([entry]);
    }

    // Whether to add the device a claimed package is for: the package must be a key package
    // whose credential names exactly that device of that user, and the page must say yes.
    async #decide(user, kp) {
        const identity = `${user}/${kp.device}`;
        let bytes;
        let info;
        try {
            bytes = fromBase64url(kp.key_package);
            info = inspect(bytes);
        } catch (e) {
            this.onEvent({ type: "denied", identity, reason: e?.message ?? "malformed" });
            return null;
        }
        if (info.wireFormat !== "key_package" || text(info.identity) !== identity) {
            this.onEvent({ type: "denied", identity, reason: "credential names another device" });
            return null;
        }
        const ask = {
            identity,
            chatSender: user,
            device: kp.device,
            fingerprint: info.fingerprint,
            keyPackageRef: info.keyPackageRef,
            lastResort: kp.last_resort === true,
        };
        let yes = false;
        try {
            yes = (await this.approve(ask)) === true;
        } catch (e) {
            this.onEvent({ type: "error", reason: `approval failed: ${e?.message ?? e}` });
        }
        if (!yes) {
            this.memo.refused.push(identity);
            this.onEvent({ type: "denied", ...ask, reason: "not approved" });
            return null;
        }
        return { bytes, identity, fingerprint: info.fingerprint, ref: info.keyPackageRef, lastResort: ask.lastResort };
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
            const posts = [];
            if (entry.welcome) {
                posts.push(this.#outbox("welcome", fromBase64url(entry.welcome)));
                this.onEvent({ type: "added", seq: frame.seq, epoch: this.group.epoch, members: this.members() });
            } else {
                this.onEvent({ type: "commit", seq: frame.seq, sender: text(this.client.identity), epoch: this.group.epoch, members: this.members() });
            }
            const done = this.#commit(posts);
            void this.#drain();
            this.#reconcileLater();
            return done;
        }
        if (this.group?.hasPendingCommit) {
            this.group.clearPendingCommit();
        }
        if (entry.directory) {
            // Lost the epoch: the next round decides again, with packages claimed afresh.
            this.onEvent({ type: "lost", seq: frame.seq, epoch: entry.epoch });
            const done = this.#commit();
            this.#reconcileLater();
            return done;
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

    // Once the group has moved, the member list last given is looked at again.
    #reconcileLater() {
        if (this.directory && this.users) {
            setTimeout(() => void this.reconcile(this.users), 0);
        }
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
        if (this.directory) {
            // One of this device's packages was spent: publish another.
            void this.directory.ensureStock().catch((e) => this.onEvent({ type: "error", reason: `stock: ${e.message}` }));
        }
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

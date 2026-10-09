// mls-room.js against an in-memory room that sequences and echoes like chat_server: who gets
// added (approval, the credential's user, the first leaf), which welcomes are taken, state
// saved before anything is posted, a commit that survives a lost send and a reload, and one
// group state per client. `node --test clients/web-mls/interop/room.test.mjs`; no server.

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

const dist = join(dirname(fileURLToPath(import.meta.url)), "..", "dist");
const mls = await import(join(dist, "mls-room.js"));
await mls.loadMls(readFileSync(join(dist, "web_mls_bg.wasm")));
const { MlsClient, MlsRoom, MlsDirectory, utf8, text, fromBase64url, toBase64url, inspect, inOrder } = mls;

const ROOM = "0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d";
const settle = () => new Promise((r) => setTimeout(r, 30));

// Sequences sends, a resent (user, id) once, and delivers every message to every socket.
class Room {
    constructor() {
        this.frames = [];
        this.sockets = [];
        this.seen = new Set();
        this.drop = new Set(); // users whose next send is lost
    }

    socket(user, deliver) {
        const s = { user, deliver };
        this.sockets.push(s);
        return (id, body) => {
            if (this.drop.delete(user)) {
                return;
            }
            if (this.seen.has(`${user}:${id}`)) {
                return;
            }
            this.seen.add(`${user}:${id}`);
            const frame = { type: "message", room: ROOM, seq: this.frames.length + 1, sender: user, id, body };
            this.frames.push(frame);
            setTimeout(() => this.sockets.forEach((t) => t.deliver(frame)), 0);
        };
    }
}

function device(room, identity, user, opts = {}) {
    const d = { client: opts.client ?? new MlsClient(utf8(identity)), received: [], events: [], saved: null, posted: [] };
    const post = room.socket(user, (f) => d.mls?.receive(f));
    d.mls = new MlsRoom({
        client: d.client,
        room: opts.roomId ?? ROOM.toUpperCase(),
        user,
        send: (id, body) => {
            d.posted.push(id);
            post(id, body);
        },
        onMessage: (m) => d.received.push(m),
        onEvent: (e) => d.events.push(e),
        onState: opts.onState ?? ((s) => (d.saved = s)),
        approveKeyPackage: opts.approve,
    });
    return d;
}

test("a key package is added only once approved, and only from the user it names", async () => {
    const room = new Room();
    const asked = [];
    const alice = device(room, "alice/laptop", "alice", {
        approve: async (kp) => {
            asked.push(kp);
            await settle();
            return kp.identity !== "bob/denied";
        },
    });
    await alice.mls.create();

    const denied = device(room, "bob/denied", "bob");
    await denied.mls.announce();
    const forged = device(room, "carol/phone", "bob"); // bob posting a credential naming carol
    await forged.mls.announce();
    await settle();
    await settle();
    assert.equal(denied.mls.joined, false);
    assert.equal(forged.mls.joined, false);
    assert.deepEqual(asked.map((a) => a.identity), ["bob/denied"], "carol's credential from bob is never asked about");
    assert.ok(alice.events.some((e) => e.type === "denied" && e.identity === "carol/phone" && e.reason === "credential names another user"));

    const bob = device(room, "bob/phone", "bob");
    await bob.mls.announce();
    for (let i = 0; i < 10 && !bob.mls.joined; i++) {
        await settle();
    }
    assert.ok(bob.mls.joined);
    const ask = asked.at(-1);
    assert.deepEqual([ask.identity, ask.chatSender], ["bob/phone", "bob"]);
    assert.equal(ask.fingerprint, bob.client.fingerprint);
    const joined = bob.events.find((e) => e.type === "joined");
    assert.deepEqual(joined.members.map((m) => [m.identity, m.fingerprint]), [
        ["alice/laptop", alice.client.fingerprint],
        ["bob/phone", bob.client.fingerprint],
    ]);
    await alice.mls.sendText("hello bob");
    await bob.mls.sendText("hello alice");
    await settle();
    assert.deepEqual(bob.received.map((m) => m.text), ["hello bob"]);
    assert.deepEqual(alice.received.map((m) => [m.sender, m.text]), [["bob/phone", "hello alice"]]);
});

test("nobody is added without an approval hook", async () => {
    const room = new Room();
    const alice = device(room, "alice", "alice");
    await alice.mls.create();
    const bob = device(room, "bob", "bob");
    await bob.mls.announce();
    await settle();
    await settle();
    assert.equal(bob.mls.joined, false);
    assert.equal(alice.mls.group.memberCount, 1);
    assert.ok(alice.events.some((e) => e.type === "denied" && e.reason === "not approved"));
});

test("a welcome is taken only after announcing, and only into the room's group", async () => {
    const room = new Room();
    const carol = device(room, "carol", "carol");
    await carol.mls.announce();
    await settle();
    const kp = fromBase64url(room.frames[0].body);
    // Eve makes another group around carol's key package and posts its welcome here.
    const eve = new MlsClient(utf8("eve"));
    const other = eve.createGroup(utf8("another-group"));
    const { welcome } = other.add([kp]);
    other.mergePendingCommit();
    const post = room.socket("eve", () => {});
    post("w1", mls.toBase64url(welcome));
    await settle();
    assert.equal(carol.mls.joined, false);
    assert.throws(() => carol.client.loadGroup(utf8("another-group")), /not_a_member/);

    // A device that never announced ignores welcomes altogether.
    const dave = device(room, "dave", "dave");
    const alice = device(room, "alice", "alice", { approve: () => true });
    await alice.mls.create();
    const kpDave = dave.client.keyPackage();
    room.socket("dave", () => {})("kp-dave", mls.toBase64url(kpDave));
    for (let i = 0; i < 10 && !alice.events.some((e) => e.type === "added"); i++) {
        await settle();
    }
    await settle();
    assert.ok(alice.events.some((e) => e.type === "added"));
    assert.equal(dave.mls.joined, false);
});

test("the state is saved before anything is posted, and a failed save posts nothing", async () => {
    const room = new Room();
    const order = [];
    let fail = false;
    const alice = device(room, "alice", "alice", {
        onState: async () => {
            order.push("save start");
            await settle();
            if (fail) {
                throw new Error("quota");
            }
            order.push("saved");
        },
    });
    await alice.mls.create();
    const id = await alice.mls.sendText("one");
    assert.deepEqual(order.slice(-2), ["save start", "saved"]);
    assert.ok(alice.posted.includes(id));
    fail = true;
    await assert.rejects(alice.mls.sendText("two"), /quota/);
    assert.equal(alice.posted.length, 1);
    assert.ok(alice.events.some((e) => e.type === "error" && /state not saved/.test(e.reason)));
});

test("a commit whose send was lost survives a reload and is resent under its id", async () => {
    const room = new Room();
    let alice = device(room, "alice", "alice", { approve: () => true });
    await alice.mls.create();
    room.drop.add("alice"); // the commit adding bob never reaches the room
    const bob = device(room, "bob", "bob");
    await bob.mls.announce();
    for (let i = 0; i < 10 && !alice.mls.group.hasPendingCommit; i++) {
        await settle();
    }
    assert.ok(alice.mls.group.hasPendingCommit);
    const lostId = alice.posted.at(-1);
    await settle();
    assert.equal(room.frames.length, 1, "only bob's key package is in the room");

    // Reload: a new client from the saved bytes; history replayed; then resume().
    const saved = alice.saved;
    alice.mls = null;
    alice = device(room, "alice", "alice", { approve: () => true, client: MlsClient.importState(saved) });
    for (const f of room.frames) {
        await alice.mls.receive(f);
    }
    await alice.mls.resume();
    assert.equal(alice.posted[0], lostId, "the same id");
    for (let i = 0; i < 10 && !bob.mls.joined; i++) {
        await settle();
    }
    assert.ok(bob.mls.joined);
    assert.equal(alice.mls.group.memberCount, 2);

    // sendFailed resends under the same id, which the room sequences once.
    const id = await bob.mls.sendText("after it all");
    await bob.mls.sendFailed(id, "unavailable");
    await settle();
    assert.equal(room.frames.filter((f) => f.id === id).length, 1);
    assert.deepEqual(alice.received.map((m) => m.text), ["after it all"]);
});

test("one group state per client however many handles, and removal is an event", async () => {
    const room = new Room();
    const alice = device(room, "alice", "alice", { approve: () => true });
    await alice.mls.create();
    const bob = device(room, "bob", "bob");
    await bob.mls.announce();
    for (let i = 0; i < 10 && !bob.mls.joined; i++) {
        await settle();
    }
    const again = alice.client.loadGroup(utf8(ROOM));
    assert.equal(again.epoch, alice.mls.group.epoch);
    const commit = again.remove(utf8("bob"));
    assert.ok(alice.mls.group.hasPendingCommit, "the room's handle sees the other's pending commit");
    again.mergePendingCommit();
    assert.equal(alice.mls.group.memberCount, 1);
    room.socket("alice", () => {})("rm", mls.toBase64url(commit));
    await settle();
    assert.ok(bob.events.some((e) => e.type === "removed"));
    assert.equal(bob.mls.joined, false);
    assert.throws(() => alice.client.createGroup(utf8(ROOM)), /rejected/);
    assert.equal(inspect(commit).contentType, "commit");
    assert.equal(text(utf8("x")), "x");
});

// --- the key directory (ADR-0101) --------------------------------------------------------------

// The chat server's key directory as one in-memory service: devices registered per user, single-
// use packages handed out once, the last resort kept, other users' devices only for users who
// share a chat (`shared`), and `replenish` to the device's own sockets. Answers come on a later
// turn, as the server's do.
class Directory {
    constructor() {
        this.devices = new Map(); // device -> {user, packages, lastResort, used, retired}
        this.sockets = [];
        this.shared = null; // null: everyone shares a chat with everyone
        this.claims = 0;
    }

    socket(user, deliver) {
        const s = { user, deliver, registered: new Set() };
        this.sockets.push(s);
        return (command) => setTimeout(() => this.#answer(s, command), 0);
    }

    #supply(device, d) {
        return {
            device,
            key_packages: d.packages.length,
            last_resort: d.lastResort ? (d.used ? "used" : "fresh") : "none",
        };
    }

    #answer(s, c) {
        const reply = (frame) => s.deliver({ ...frame, id: c.id });
        const refuse = (reason) => reply({ type: "error", reason });
        const own = (device) => {
            const d = this.devices.get(device);
            if (!d || d.user !== s.user) {
                refuse("unknown_device");
                return null;
            }
            if (d.retired) {
                refuse("device_retired");
                return null;
            }
            return d;
        };
        const mayRead = (user) => user === s.user || this.shared === null || this.shared.has(`${s.user}:${user}`);
        switch (c.type) {
            case "register_device": {
                if (!this.devices.has(c.device)) {
                    this.devices.set(c.device, { user: s.user, packages: [], lastResort: null, used: false, retired: false });
                }
                const d = own(c.device);
                if (d) {
                    s.registered.add(c.device);
                    reply({ type: "device_registered", ...this.#supply(c.device, d) });
                }
                return;
            }
            case "publish_key_packages": {
                const d = own(c.device);
                if (d) {
                    d.packages.push(...(c.key_packages ?? []));
                    if (c.last_resort) {
                        d.lastResort = c.last_resort;
                        d.used = false;
                    }
                    reply({ type: "key_packages_published", ...this.#supply(c.device, d) });
                }
                return;
            }
            case "retire_device": {
                const d = own(c.device);
                if (d) {
                    Object.assign(d, { retired: true, packages: [], lastResort: null });
                    reply({ type: "device_retired", device: c.device });
                }
                return;
            }
            case "devices": {
                if (!mayRead(c.user)) {
                    refuse("not_shared");
                    return;
                }
                const devices = [...this.devices].filter(([, d]) => d.user === c.user && !d.retired)
                    .map(([device, d]) => (c.user === s.user ? this.#supply(device, d) : { device }));
                reply({ type: "devices", user: c.user, devices });
                return;
            }
            case "claim_key_packages": {
                if (!mayRead(c.user)) {
                    refuse("not_shared");
                    return;
                }
                this.claims += 1;
                const live = [...this.devices].filter(([, d]) => d.user === c.user && !d.retired).map(([id]) => id);
                const wanted = c.devices ?? live;
                const answer = { type: "key_packages", user: c.user, key_packages: [], exhausted: [], gone: [], unavailable: [] };
                for (const device of wanted) {
                    if (!live.includes(device)) {
                        answer.gone.push(device);
                        continue;
                    }
                    const d = this.devices.get(device);
                    if (d.packages.length) {
                        answer.key_packages.push({ device, key_package: d.packages.shift(), last_resort: false });
                    } else if (d.lastResort) {
                        d.used = true;
                        answer.key_packages.push({ device, key_package: d.lastResort, last_resort: true });
                    } else {
                        answer.exhausted.push(device);
                    }
                    if (d.packages.length <= 1) {
                        for (const t of this.sockets) {
                            if (t.registered.has(device)) {
                                t.deliver({ type: "replenish", device });
                            }
                        }
                    }
                }
                reply(answer);
                return;
            }
            default:
                refuse("malformed");
        }
    }
}

let deviceCount = 0;
function newDeviceId() {
    deviceCount += 1;
    return `01a0eb86-6cca-7dce-84cc-${String(deviceCount).padStart(12, "0")}`;
}

// A device with the directory: its identity is `<user>/<device id>`, as the convention says.
function directoryDevice(room, dir, user, opts = {}) {
    const id = opts.deviceId ?? newDeviceId();
    const client = opts.client ?? new MlsClient(utf8(`${user}/${id}`));
    const d = { id, identity: `${user}/${id}`, client, received: [], events: [], saved: null, saves: 0 };
    const save = inOrder(async (s) => {
        d.saved = s;
        d.saves += 1;
    });
    const sendDir = dir.socket(user, (f) => d.directory.receive(f));
    d.directory = new MlsDirectory({ client, device: id, send: sendDir, onState: save, target: opts.target ?? 4,
        onEvent: (e) => d.events.push({ from: "directory", ...e }) });
    const post = room.socket(user, (f) => d.mls?.receive(f));
    d.mls = new MlsRoom({
        client,
        room: ROOM,
        user,
        send: post,
        onMessage: (m) => d.received.push(m),
        onEvent: (e) => d.events.push(e),
        onState: save,
        approveKeyPackage: opts.approve,
        directory: d.directory,
    });
    return d;
}

async function until(test, what) {
    for (let i = 0; i < 100; i++) {
        if (test()) {
            return;
        }
        await settle();
    }
    assert.fail(what);
}

test("devices found through the directory join, every device of every user", async () => {
    const room = new Room();
    const dir = new Directory();
    const asked = [];
    const approve = (kp) => {
        asked.push(kp);
        return true;
    };
    const phone = directoryDevice(room, dir, "alice", { approve });
    await phone.directory.ensureStock();
    await phone.mls.create();
    const laptop = directoryDevice(room, dir, "alice");
    const bob = directoryDevice(room, dir, "bob");
    await laptop.mls.announce();
    await bob.mls.announce();
    assert.equal(room.frames.length, 0, "nothing goes to the room to ask");
    // Published while announcing: single-use packages and a last resort, private keys saved first.
    const supply = await bob.directory.devices("bob");
    assert.deepEqual(supply.map((s) => [s.key_packages, s.last_resort]), [[4, "fresh"]]);
    assert.ok(bob.saves >= 1);

    await phone.mls.reconcile(["alice", "bob"]);
    await until(() => laptop.mls.joined && bob.mls.joined, "laptop and bob never joined");
    assert.deepEqual(asked.map((a) => a.identity).sort(), [laptop.identity, bob.identity].sort());
    const askedBob = asked.find((a) => a.identity === bob.identity);
    assert.equal(askedBob.device, bob.id);
    assert.equal(askedBob.chatSender, "bob");
    assert.equal(askedBob.fingerprint, bob.client.fingerprint);
    assert.equal(askedBob.lastResort, false);
    // One commit added both: one welcome in the room.
    const kinds = room.frames.map((f) => inspect(fromBase64url(f.body)).wireFormat);
    assert.deepEqual(kinds, ["private_message", "welcome"]);
    assert.equal(phone.mls.group.memberCount, 3);

    await laptop.mls.sendText("from alice's laptop");
    await bob.mls.sendText("from bob");
    await settle();
    assert.deepEqual(phone.received.map((m) => m.text).sort(), ["from alice's laptop", "from bob"]);
    assert.deepEqual(laptop.received.map((m) => m.text), ["from bob"]);
    assert.deepEqual(bob.received.map((m) => m.text), ["from alice's laptop"]);

    // A second round finds nothing to do and claims nothing.
    const claims = dir.claims;
    await phone.mls.reconcile(["alice", "bob"]);
    assert.equal(dir.claims, claims);
});

test("a package whose credential names another device is never added", async () => {
    const room = new Room();
    const dir = new Directory();
    const asked = [];
    const alice = directoryDevice(room, dir, "alice", { approve: (kp) => asked.push(kp) && true });
    await alice.mls.create();
    // Mallory registers a device of his own and publishes a package made for bob's identity.
    const mallory = directoryDevice(room, dir, "mallory");
    await mallory.directory.ensureStock();
    const forger = new MlsClient(utf8(`bob/${mallory.id}`));
    dir.devices.get(mallory.id).packages = [toBase64url(forger.keyPackage())];
    await alice.mls.reconcile(["alice", "mallory"]);
    assert.equal(asked.length, 0);
    assert.ok(alice.events.some((e) => e.type === "denied" && e.reason === "credential names another device"));
    assert.equal(alice.mls.group.memberCount, 1);
});

test("an unapproved device is asked about once, and nobody without the hook", async () => {
    const room = new Room();
    const dir = new Directory();
    let asked = 0;
    const alice = directoryDevice(room, dir, "alice", { approve: () => { asked += 1; return false; } });
    await alice.mls.create();
    const bob = directoryDevice(room, dir, "bob");
    await bob.mls.announce();
    await alice.mls.reconcile(["alice", "bob"]);
    await alice.mls.reconcile(["alice", "bob"]);
    assert.equal(asked, 1);
    assert.equal(bob.mls.joined, false);

    const carolRoom = new Room();
    const carolDir = new Directory();
    const carol = directoryDevice(carolRoom, carolDir, "carol");
    await carol.mls.create();
    const dave = directoryDevice(carolRoom, carolDir, "dave");
    await dave.mls.announce();
    await carol.mls.reconcile(["carol", "dave"]);
    assert.equal(dave.mls.joined, false);
    assert.ok(carol.events.some((e) => e.type === "denied" && e.reason === "not approved"));
});

test("a retired device and a user taken off the list are removed", async () => {
    const room = new Room();
    const dir = new Directory();
    const phone = directoryDevice(room, dir, "alice", { approve: () => true });
    await phone.mls.create();
    const laptop = directoryDevice(room, dir, "alice");
    const bob = directoryDevice(room, dir, "bob");
    await laptop.mls.announce();
    await bob.mls.announce();
    await phone.mls.reconcile(["alice", "bob"]);
    await until(() => laptop.mls.joined && bob.mls.joined, "never joined");

    await laptop.directory.retire();
    await phone.mls.reconcile(["alice"]);
    await until(() => phone.mls.group.memberCount === 1, "the laptop and bob were not both removed");
    await until(() => laptop.events.some((e) => e.type === "removed") && bob.events.some((e) => e.type === "removed"),
        "the removed devices never heard");
    assert.equal(bob.mls.joined, false);
    assert.equal(laptop.mls.joined, false);
    assert.ok(phone.events.filter((e) => e.type === "removing").length === 2);
});

test("a device out of single-use packages joins through its last resort, then publishes more", async () => {
    const room = new Room();
    const dir = new Directory();
    const alice = directoryDevice(room, dir, "alice", { approve: () => true });
    await alice.mls.create();
    const bob = directoryDevice(room, dir, "bob", { target: 1 });
    await bob.mls.announce();
    dir.devices.get(bob.id).packages = []; // spent elsewhere
    await alice.mls.reconcile(["alice", "bob"]);
    await until(() => bob.mls.joined, "bob never joined through his last resort");
    assert.ok(alice.events.some((e) => e.type === "adding" && e.lastResort === true));
    // Told to replenish, and on joining: the used last resort is replaced and packages added.
    await until(() => {
        const d = dir.devices.get(bob.id);
        return d.lastResort && !d.used && d.packages.length >= 1;
    }, "bob never published again");
});

test("directory requests wait out a rate limit and fail on a final refusal", async () => {
    const sent = [];
    const client = new MlsClient(utf8("alice/01a0eb86-6cca-7dce-84cc-3bb47615f9fd"));
    const d = new MlsDirectory({ client, device: "01a0eb86-6cca-7dce-84cc-3bb47615f9fd", send: (c) => sent.push(c) });
    const answer = d.devices("bob");
    d.receive({ type: "error", reason: "rate_limited", retry_after_ms: 5, id: sent[0].id });
    await until(() => sent.length === 2, "never asked again");
    assert.notEqual(sent[1].id, sent[0].id, "a fresh request id");
    assert.equal(d.receive({ type: "devices", user: "bob", devices: [{ device: "x" }], id: sent[1].id }), true);
    assert.deepEqual(await answer, [{ device: "x" }]);

    const refused = d.claim("bob");
    d.receive({ type: "error", reason: "not_shared", id: sent[2].id });
    await assert.rejects(refused, /not_shared/);
    assert.equal(d.receive({ type: "message", id: "other" }), false, "not the directory's");
    assert.throws(() => new MlsDirectory({ client, device: "Laptop", send: () => {} }), /invalid_argument/);
});

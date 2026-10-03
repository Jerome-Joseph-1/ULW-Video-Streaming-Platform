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
const { MlsClient, MlsRoom, utf8, text, fromBase64url, inspect } = mls;

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

// The browser client and the FFI bridge in one MLS group, through a real chat_server on a
// scratch Postgres database: the WebAssembly build in dist/ (through mls-room.js, as a page uses
// it) and ffi_peer, the bridge's own code, each on its own chat socket as its own user. Run by
// run.sh, which builds ffi_peer; see there for the environment.
//
// Room one: the browser starts the group and adds the native device; then a second browser
// device announces itself and the first adds it, and the native device follows the commit.
// Room two: the native device starts the group, adds a browser device, and later a second one,
// whose commit the first browser device follows. Room three: nothing asks through the room; the
// key directory (ADR-0102) holds every device's packages, and a browser device adds its user's
// second browser device and the native device in one commit. Everyone reads everyone, the bodies
// stored are the bytes each side made, and a browser device saved to bytes and restored mid-way
// keeps reading. Exits non-zero on the first failure.

import assert from "node:assert/strict";
import { randomUUID } from "node:crypto";
import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

import { NativeDevice, Socket, log, psql, run, scratchDatabase, startChat } from "./harness.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const dist = join(here, "..", "dist");
const mls = await import(join(dist, "mls-room.js"));
await mls.loadMls(readFileSync(join(dist, "web_mls_bg.wasm")));
const { MlsClient, MlsRoom, utf8, text, fromBase64url, inspect } = mls;

// --- a browser device: the WebAssembly client behind MlsRoom ---------------------------------

function browserDevice(socket, room, client) {
    const device = { socket, received: [], events: [], state: null };
    device.mls = new MlsRoom({
        client,
        room,
        user: socket.user,
        send: (id, body) => socket.send(room, id, body),
        // The checks here are about the wire, not who may join: every device is welcome.
        approveKeyPackage: () => true,
        onMessage: (m) => device.received.push(m),
        onEvent: (e) => device.events.push(e),
        onState: (s) => (device.state = s),
    });
    socket.onFrame = (f) => {
        if (f.type === "message" && f.room === room) {
            device.mls.receive(f);
        }
    };
    device.heard = async (needle, ms = 10000) => {
        const until = Date.now() + ms;
        while (Date.now() < until) {
            const m = device.received.find((r) => r.text === needle);
            if (m) {
                return m;
            }
            await new Promise((r) => setTimeout(r, 20));
        }
        throw new Error(`${socket.user} never decrypted "${needle}"; events ${JSON.stringify(device.events)}`);
    };
    device.until = async (test, what, ms = 10000) => {
        const until = Date.now() + ms;
        while (Date.now() < until) {
            if (test()) {
                return;
            }
            await new Promise((r) => setTimeout(r, 20));
        }
        throw new Error(`${socket.user}: ${what}; events ${JSON.stringify(device.events)}`);
    };
    return device;
}

// A message whose stored body has the size chat_e2ee_test checks the bridge's against: an
// MLSMessage holding a PrivateMessage for a 36-byte group id is its plaintext and 166 bytes.
function longText(who) {
    return `${who}: ${"see you at the north entrance at half past six; ".repeat(14)}`.slice(0, 600);
}

function assertKeyPackageForm(bytes) {
    // MLSMessage: version mls10 (1), wire_format mls_key_package (5); KeyPackage: version mls10,
    // cipher_suite 1, MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519.
    assert.deepEqual([...bytes.subarray(0, 8)], [0, 1, 0, 5, 0, 1, 0, 1]);
}

// --- room one: the browser starts the group --------------------------------------------------

async function roomOne(db) {
    const room = randomUUID();
    psql(db, `INSERT INTO chat_rooms (room_id, kind) VALUES ('${room}', 'group_chat')`);
    for (const u of ["web-a", "ffi-b", "web-c"]) {
        psql(db, `INSERT INTO chat_members (room_id, user_id) VALUES ('${room}', '${u}')`);
    }
    const [sa, sb, sc] = ["web-a", "ffi-b", "web-c"].map((u) => new Socket(u));
    await Promise.all([sa, sb, sc].map((s) => s.open()));
    await Promise.all([sa, sb, sc].map((s) => s.join(room)));

    const a = browserDevice(sa, room, new MlsClient(utf8("web-a")));
    await a.mls.create();
    const b = await NativeDevice.create("ffi-b");

    // The native device asks to be added; the browser adds it and posts the welcome.
    const kp = await b.keyPackage();
    assertKeyPackageForm(kp);
    assert.equal(text(inspect(kp).identity), "ffi-b", "the browser reads the bridge's key package");
    await sb.sendAndWait(room, kp);
    const welcome = await sb.message(room, (f) => f.sender === "web-a" &&
        inspect(fromBase64url(f.body)).wireFormat === "welcome", "a welcome from web-a");
    await b.call("join", Buffer.from(fromBase64url(welcome.body)).toString("hex"));
    assert.equal(await b.number("epoch"), 1);
    assert.equal(await b.number("members"), 2);
    log("room one: the bridge joined from the browser's welcome");

    // Both ways, at the size whose framing the C++ test pins.
    const fromA = longText("web-a");
    await a.mls.sendText(fromA);
    const seen = await sb.message(room, (f) => f.sender === "web-a" &&
        inspect(fromBase64url(f.body)).contentType === "application", "web-a's message");
    const body = fromBase64url(seen.body);
    assert.equal(body.length, fromA.length + 166, "the bridge's framing, byte for byte");
    assert.deepEqual(await b.process(body), { kind: "application", plaintext: fromA });
    const fromB = longText("ffi-b");
    const sealed = await b.encrypt(fromB);
    assert.equal(sealed.length, fromB.length + 166);
    await sb.sendAndWait(room, sealed);
    assert.equal((await a.heard(fromB)).sender, "ffi-b");
    log("room one: browser and bridge read each other");

    // A second browser device announces itself; the first adds it; the bridge follows.
    const c = browserDevice(sc, room, new MlsClient(utf8("web-c")));
    await c.mls.announce();
    await c.until(() => c.mls.joined, "web-c never joined");
    const commit = sb.frames.find((f) => f.type === "message" && f.sender === "web-a" &&
        inspect(fromBase64url(f.body)).contentType === "commit" &&
        inspect(fromBase64url(f.body)).epoch === 1);
    assert.ok(commit, "web-a's commit adding web-c");
    assert.equal((await b.process(fromBase64url(commit.body))).kind, "commit");
    assert.equal(await b.number("epoch"), 2);
    assert.equal(await b.number("members"), 3);
    const fromB2 = "ffi-b to both browsers";
    await sb.sendAndWait(room, await b.encrypt(fromB2));
    await a.heard(fromB2);
    await c.heard(fromB2);
    await c.mls.sendText("web-c to everyone");
    const fromC = await sb.message(room, (f) => f.sender === "web-c" &&
        inspect(fromBase64url(f.body)).contentType === "application", "web-c's message");
    assert.equal((await b.process(fromBase64url(fromC.body))).plaintext, "web-c to everyone");
    await a.heard("web-c to everyone");
    log("room one: a second browser device added by the first; the bridge followed the commit");

    // What the room stored is what was sent, byte for byte.
    const stored = psql(db, `SELECT encode(body, 'hex') FROM chat_messages WHERE room_id = '${room}' ORDER BY seq`)
        .trim().split("\n");
    const sent = sb.frames.filter((f) => f.type === "message" && f.room === room)
        .map((f) => Buffer.from(fromBase64url(f.body)).toString("hex"));
    assert.deepEqual(stored, sent);
    for (const s of [sa, sb, sc]) {
        s.ws.close();
    }
    return stored.length;
}

// --- room two: the bridge starts the group ---------------------------------------------------

async function roomTwo(db) {
    const room = randomUUID();
    psql(db, `INSERT INTO chat_rooms (room_id, kind) VALUES ('${room}', 'group_chat')`);
    for (const u of ["ffi-b", "web-a", "web-c"]) {
        psql(db, `INSERT INTO chat_members (room_id, user_id) VALUES ('${room}', '${u}')`);
    }
    const [sb, sa, sc] = ["ffi-b", "web-a", "web-c"].map((u) => new Socket(u));
    await Promise.all([sa, sb, sc].map((s) => s.open()));
    await Promise.all([sa, sb, sc].map((s) => s.join(room)));

    const b = await NativeDevice.create("ffi-b");
    await b.call("create", room);

    // The bridge adds whichever browser device announces itself, and merges once the room has
    // sequenced its commit.
    const addFrom = async (announcer) => {
        const kpFrame = await sb.message(room, (f) => f.sender === announcer &&
            inspect(fromBase64url(f.body)).wireFormat === "key_package", `${announcer}'s key package`);
        const kp = fromBase64url(kpFrame.body);
        assertKeyPackageForm(kp);
        const [commit, welcome] = await b.call("add", Buffer.from(kp).toString("hex"));
        await sb.sendAndWait(room, Buffer.from(commit, "hex"));
        await b.call("merge");
        await sb.sendAndWait(room, Buffer.from(welcome, "hex"));
    };

    let a = browserDevice(sa, room, new MlsClient(utf8("web-a")));
    await a.mls.announce();
    await addFrom("web-a");
    await a.until(() => a.mls.joined, "web-a never joined from the bridge's welcome");
    assert.equal(a.mls.group.epoch, 1);
    log("room two: the browser joined from the bridge's welcome");

    const fromB = longText("ffi-b");
    await sb.sendAndWait(room, await b.encrypt(fromB));
    assert.equal((await a.heard(fromB)).sender, "ffi-b");
    await a.mls.sendText("web-a back to ffi-b");
    const fromA = await sb.message(room, (f) => f.sender === "web-a" &&
        inspect(fromBase64url(f.body)).contentType === "application", "web-a's message");
    assert.deepEqual(await b.process(fromBase64url(fromA.body)), { kind: "application", plaintext: "web-a back to ffi-b" });

    // The browser device is saved to bytes and restored, as a page reload from IndexedDB.
    const saved = a.state;
    assert.ok(saved instanceof Uint8Array && saved.length > 0);
    sa.onFrame = null;
    a = browserDevice(sa, room, MlsClient.importState(saved));
    assert.ok(a.mls.joined, "the restored device is still in the group");
    await sb.sendAndWait(room, await b.encrypt("after the reload"));
    await a.heard("after the reload");
    log("room two: a browser device restored from exported state keeps reading");

    // A second browser device; the first follows the bridge's commit.
    const c = browserDevice(sc, room, new MlsClient(utf8("web-c")));
    await c.mls.announce();
    await addFrom("web-c");
    await c.until(() => c.mls.joined, "web-c never joined");
    await a.until(() => a.mls.group.epoch === 2, "web-a never followed the bridge's commit");
    assert.ok(a.events.some((e) => e.type === "commit" && e.sender === "ffi-b"));
    await sb.sendAndWait(room, await b.encrypt("three of us"));
    await a.heard("three of us");
    await c.heard("three of us");
    await c.mls.sendText("web-c here");
    await a.heard("web-c here");
    const fromC = await sb.message(room, (f) => f.sender === "web-c" &&
        inspect(fromBase64url(f.body)).contentType === "application", "web-c's message");
    assert.equal((await b.process(fromBase64url(fromC.body))).plaintext, "web-c here");
    log("room two: the browser followed the bridge's commit adding a second browser device");

    // No plaintext reached the database.
    const bodies = psql(db, `SELECT encode(body, 'escape') FROM chat_messages WHERE room_id = '${room}'`);
    for (const plain of [fromB, "web-a back to ffi-b", "after the reload", "three of us", "web-c here"]) {
        assert.ok(!bodies.includes(plain), `plaintext stored: ${plain}`);
    }
    for (const s of [sa, sb, sc]) {
        s.ws.close();
    }
}

// --- room three: devices found through the key directory (ADR-0102) ---------------------------

// A browser device with the directory: its identity is `<user>/<device id>`.
function directoryDevice(socket, room) {
    const id = randomUUID();
    const client = new MlsClient(utf8(`${socket.user}/${id}`));
    const save = mls.inOrder(() => {});
    const directory = new mls.MlsDirectory({ client, device: id, send: (c) => socket.ws.send(JSON.stringify(c)), onState: save });
    const device = browserDevice(socket, room, client);
    // browserDevice made its room without the directory; this one has it.
    device.mls = new MlsRoom({
        client,
        room,
        user: socket.user,
        directory,
        send: (i, body) => socket.send(room, i, body),
        approveKeyPackage: () => true,
        onMessage: (m) => device.received.push(m),
        onEvent: (e) => device.events.push(e),
        onState: save,
    });
    socket.onFrame = (f) => {
        if (directory.receive(f)) {
            return;
        }
        if (f.type === "message" && f.room === room) {
            device.mls.receive(f);
        }
    };
    Object.assign(device, { id, directory, identity: `${socket.user}/${id}` });
    return device;
}

async function roomThree(db) {
    const room = randomUUID();
    psql(db, `INSERT INTO chat_rooms (room_id, kind) VALUES ('${room}', 'group_chat')`);
    for (const u of ["web-a", "ffi-b"]) {
        psql(db, `INSERT INTO chat_members (room_id, user_id) VALUES ('${room}', '${u}')`);
    }
    // web-a has two browser devices, each on its own socket.
    const [sa1, sa2, sb] = ["web-a", "web-a", "ffi-b"].map((u) => new Socket(u));
    await Promise.all([sa1, sa2, sb].map((s) => s.open()));
    await Promise.all([sa1, sa2, sb].map((s) => s.join(room)));

    const a1 = directoryDevice(sa1, room);
    const a2 = directoryDevice(sa2, room);
    await a1.directory.ensureStock();
    await a1.mls.create();
    await a2.mls.announce();

    // The native device registers and publishes two packages through the same commands.
    const bId = randomUUID();
    const b = await NativeDevice.create(`ffi-b/${bId}`);
    const ask = async (command, answer) => {
        const id = randomUUID();
        sb.ws.send(JSON.stringify({ ...command, id }));
        const frame = await sb.wait((f) => f.id === id && (f.type === answer || f.type === "error"), answer);
        assert.equal(frame.type, answer, JSON.stringify(frame));
        return frame;
    };
    await ask({ type: "register_device", device: bId }, "device_registered");
    const packages = [await b.keyPackage(), await b.keyPackage()];
    const published = await ask({ type: "publish_key_packages", device: bId,
        key_packages: packages.map((p) => Buffer.from(p).toString("base64url")) }, "key_packages_published");
    assert.equal(published.key_packages, 2);

    // web-a's first device adds every device it finds: its sibling and the native one.
    await a1.mls.reconcile(["web-a", "ffi-b"]);
    await a2.until(() => a2.mls.joined, "web-a's second device never joined");
    const welcome = await sb.message(room, (f) => f.sender === "web-a" &&
        inspect(fromBase64url(f.body)).wireFormat === "welcome", "the welcome");
    await b.call("join", Buffer.from(fromBase64url(welcome.body)).toString("hex"));
    assert.equal(await b.number("members"), 3);
    assert.equal(a1.mls.group.memberCount, 3);
    assert.deepEqual(a1.mls.members().map((m) => m.identity).sort(), [a1.identity, a2.identity, `ffi-b/${bId}`].sort());
    log("room three: a browser device added its sibling and the bridge's device from the key directory");

    await sb.sendAndWait(room, await b.encrypt("from the bridge"));
    await a1.heard("from the bridge");
    await a2.heard("from the bridge");
    await a2.mls.sendText("from web-a's second device");
    const fromA2 = await sb.message(room, (f) => f.sender === "web-a" &&
        inspect(fromBase64url(f.body)).contentType === "application", "web-a's message");
    assert.equal((await b.process(fromBase64url(fromA2.body))).plaintext, "from web-a's second device");
    await a1.heard("from web-a's second device");
    // One of the bridge's packages was spent, the other is still there.
    assert.equal(psql(db, `SELECT count(*) FROM key_packages WHERE device_id = '${bId}'`).trim(), "1");
    log("room three: every device reads every other's messages");
    for (const s of [sa1, sa2, sb]) {
        s.ws.close();
    }
}

await run(async () => {
    const db = scratchDatabase();
    const chatOutput = await startChat(db);
    try {
        const stored = await roomOne(db);
        await roomTwo(db);
        await roomThree(db);
        log(`passed: ${stored} room-one bodies stored exactly as sent; ciphersuite 1 on both sides`);
    } catch (e) {
        console.error(chatOutput().slice(-4000));
        throw e;
    }
});

// example.html in two Chromium contexts (each its own user, cookie jar and IndexedDB) against a
// real chat_server, with the FFI bridge's device in the room too: the first browser starts the
// group and adds the second and then the native device, everyone reads everyone, and the second
// browser reloads and comes back from IndexedDB still in the group. Run by run.sh --browser.

import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { randomUUID } from "node:crypto";
import { createReadStream, existsSync, readFileSync } from "node:fs";
import { createServer } from "node:http";
import { createRequire } from "node:module";
import { dirname, extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";

import { NativeDevice, PORT, Socket, cleanups, log, mint, psql, run, scratchDatabase, startChat } from "./harness.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const site = join(here, "..");
const PAGE_PORT = PORT + 9;
const origin = `http://127.0.0.1:${PAGE_PORT}`;

const mls = await import(join(site, "dist", "mls-room.js"));
await mls.loadMls(readFileSync(join(site, "dist", "web_mls_bg.wasm")));

function playwright() {
    try {
        return createRequire(import.meta.url)("playwright");
    } catch {
        const root = execFileSync("npm", ["root", "-g"], { encoding: "utf8" }).trim();
        return createRequire(join(root, "noop.js"))("playwright");
    }
}

function serveSite() {
    const types = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm", ".ts": "text/plain" };
    const server = createServer((req, res) => {
        const path = normalize(join(site, decodeURIComponent(new URL(req.url, origin).pathname)));
        if (!path.startsWith(site) || !existsSync(path)) {
            res.writeHead(404).end();
            return;
        }
        res.writeHead(200, { "content-type": types[extname(path)] ?? "application/octet-stream" });
        createReadStream(path).pipe(res);
    });
    server.listen(PAGE_PORT, "127.0.0.1");
    cleanups.push(() => server.close());
}

async function openDevice(browser, room, user) {
    const context = await browser.newContext();
    const page = await context.newPage();
    page.on("pageerror", (e) => log(`${user} page error:`, e.message));
    const hash = new URLSearchParams({ chat: `ws://127.0.0.1:${PORT}/rt`, room, device: user, token: mint(user) });
    await page.goto(`${origin}/example.html#${hash}`);
    await page.click("#connect");
    await page.waitForFunction(() => /Connected as|In the group/.test(document.getElementById("status").textContent));
    return page;
}

async function say(page, text) {
    await page.fill("#text", text);
    await page.click("#sendButton");
}

async function sees(page, text) {
    await page.waitForFunction((t) => document.getElementById("log").textContent.includes(t), text, { timeout: 10000 });
}

async function inGroup(page, members) {
    await page.waitForFunction(
        (m) => document.getElementById("status").textContent.includes(`${m} members`), members, { timeout: 10000 });
}

run(async () => {
    const db = scratchDatabase();
    await startChat(db, { ULW_ALLOWED_ORIGINS: origin });
    serveSite();
    const room = randomUUID();
    psql(db, `INSERT INTO chat_rooms (room_id, kind) VALUES ('${room}', 'group_chat')`);
    for (const u of ["alice", "bob", "ffi-carol"]) {
        psql(db, `INSERT INTO chat_members (room_id, user_id) VALUES ('${room}', '${u}')`);
    }

    const browser = await playwright().chromium.launch();
    cleanups.push(() => browser.close());
    const alice = await openDevice(browser, room, "alice");
    const bob = await openDevice(browser, room, "bob");

    await alice.click("#create");
    await inGroup(alice, 1);
    await bob.click("#announce");
    await inGroup(bob, 2);
    log("bob's browser joined alice's group through the room");
    await say(alice, "hello bob, from alice's browser");
    await sees(bob, "hello bob, from alice's browser");
    await say(bob, "hi alice");
    await sees(alice, "hi alice");
    log("two browser contexts exchanged encrypted messages");

    // The bridge's device asks to join; alice's page adds it.
    const sc = new Socket("ffi-carol");
    await sc.open();
    await sc.join(room);
    const carol = await NativeDevice.create("ffi-carol");
    await sc.sendAndWait(room, await carol.keyPackage());
    const welcome = await sc.message(room, (f) => f.sender === "alice" &&
        mls.inspect(mls.fromBase64url(f.body)).wireFormat === "welcome", "alice's welcome");
    await carol.call("join", Buffer.from(mls.fromBase64url(welcome.body)).toString("hex"));
    await inGroup(bob, 3);
    await say(alice, "three now");
    const m = await sc.message(room, (f) => f.sender === "alice" && f.seq > welcome.seq &&
        mls.inspect(mls.fromBase64url(f.body)).contentType === "application", "alice's message");
    assert.equal((await carol.process(mls.fromBase64url(m.body))).plaintext, "three now");
    await sees(bob, "three now");
    await sc.sendAndWait(room, await carol.encrypt("native says hi"));
    await sees(alice, "native says hi");
    await sees(bob, "native says hi");
    log("the bridge's device joined from the page's welcome and reads and writes");

    // Bob reloads: his device comes back from IndexedDB, still in the group.
    await bob.reload();
    await bob.click("#connect");
    await inGroup(bob, 3);
    await say(alice, "after bob's reload");
    await sees(bob, "after bob's reload");
    log("a reloaded page restored its device from IndexedDB");

    const bodies = psql(db, `SELECT encode(body, 'escape') FROM chat_messages WHERE room_id = '${room}'`);
    for (const plain of ["hello bob", "hi alice", "three now", "native says hi", "after bob's reload"]) {
        assert.ok(!bodies.includes(plain), `plaintext stored: ${plain}`);
    }
    sc.ws.close();
    log("passed");
});

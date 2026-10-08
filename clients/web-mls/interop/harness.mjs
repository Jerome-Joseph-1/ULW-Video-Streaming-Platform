// What the interop checks share: a scratch database, a dev key set and one chat_server, chat
// sockets, and the native device (ffi_peer, the FFI bridge's own code, over a pipe).

import assert from "node:assert/strict";
import { spawn, execFileSync } from "node:child_process";
import { generateKeyPairSync, randomBytes, randomUUID, sign } from "node:crypto";
import { mkdtempSync, rmSync, writeFileSync, chmodSync } from "node:fs";
import { createInterface } from "node:readline";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, "..", "..", "..");
const toBase64url = (bytes) => Buffer.from(bytes).toString("base64url");

const PEER = process.env.ULW_FFI_PEER ?? join(here, "ffi_peer");
const PG = process.env.ULW_TEST_DATABASE_URL ??
    "postgresql://postgres:testtest123@127.0.0.1:55432/postgres";
const CHAT_BIN = process.env.ULW_CHAT_BIN ?? "";
// main as of 404c008, by digest; ULW_CHAT_IMAGE or ULW_CHAT_BIN for another.
const CHAT_IMAGE = process.env.ULW_CHAT_IMAGE ??
    "ghcr.io/jerome-joseph-1/ulw-chat@sha256:ba893a0f2e564074f5fb532cee054047f91185d839d7d1338ac972bd04e52e4f";
export const PORT = Number(process.env.ULW_CHAT_PORT ?? 9171);
const ISSUER = "https://auth.example.com";

export const cleanups = [];
export const log = (...a) => console.log("[interop]", ...a);

// --- the server: a scratch database, a dev key set, one chat_server -----------------------

export function psql(url, sql) {
    return execFileSync("psql", [url, "-v", "ON_ERROR_STOP=1", "-qAt", "-c", sql], {
        encoding: "utf8",
    });
}

export function scratchDatabase() {
    const name = `ulw_mlswasm_${randomBytes(6).toString("hex")}`;
    psql(PG, `CREATE DATABASE ${name}`);
    const url = PG.replace(/\/[^/?]*(\?|$)/, `/${name}$1`);
    cleanups.push(() => psql(PG, `DROP DATABASE IF EXISTS ${name} WITH (FORCE)`));
    const migrations = join(repo, "migrations");
    for (const file of execFileSync("ls", [migrations], { encoding: "utf8" }).split("\n")) {
        if (file.endsWith(".sql")) {
            execFileSync("psql", [url, "-v", "ON_ERROR_STOP=1", "-q", "-f", join(migrations, file)]);
        }
    }
    log("database", name);
    return url;
}

const { publicKey, privateKey } = generateKeyPairSync("ed25519");
const kid = randomUUID();

function jwks() {
    const x = publicKey.export({ format: "jwk" }).x;
    return JSON.stringify({ keys: [{ kty: "OKP", crv: "Ed25519", use: "sig", alg: "EdDSA", kid, x }] });
}

export function mint(user) {
    const b64 = (o) => Buffer.from(JSON.stringify(o)).toString("base64url");
    const now = Math.floor(Date.now() / 1000);
    const head = b64({ alg: "EdDSA", typ: "JWT", kid });
    const body = b64({ iss: ISSUER, aud: "ulw-dev", sub: user, iat: now, nbf: now, exp: now + 600 });
    const sig = sign(null, Buffer.from(`${head}.${body}`), privateKey).toString("base64url");
    return `${head}.${body}.${sig}`;
}

export async function startChat(databaseUrl, extraEnv = {}) {
    const dir = mkdtempSync(join(tmpdir(), "ulw-mlswasm-"));
    chmodSync(dir, 0o755);
    cleanups.push(() => rmSync(dir, { recursive: true, force: true }));
    const jwksFile = join(dir, "jwks.json");
    writeFileSync(jwksFile, jwks(), { mode: 0o644 });
    const env = {
        ULW_NODE_ID: "chat-mlswasm",
        ULW_DEV_LOOPBACK_NODES: "1",
        ULW_NODE_SECRET: randomBytes(32).toString("hex"),
        ULW_DATABASE_URL: databaseUrl,
        ULW_DEV_JWKS_FILE: CHAT_BIN ? jwksFile : "/jwks/jwks.json",
        ULW_DEV_MODE: "1",
        JWT_ISSUER: ISSUER,
        // Named, since the default changed (ADR-0088): images from before it expect another.
        JWT_AUDIENCE: "ulw-dev",
        ULW_REACTOR: "epoll",
        ULW_ALLOW_ROOT: "1",
        ULW_LISTEN_PORT: String(PORT),
        ULW_NODE_ADDRESS: `127.0.0.1:${PORT + 1}`,
        ...extraEnv,
    };
    let child;
    if (CHAT_BIN) {
        child = spawn(CHAT_BIN, [], { env: { ...process.env, ...env }, stdio: ["ignore", "pipe", "pipe"] });
    } else {
        const name = `ulw-mlswasm-chat-${randomBytes(4).toString("hex")}`;
        const args = ["run", "--rm", "--name", name, "--network", "host", "-v", `${dir}:/jwks:ro`];
        for (const [k, v] of Object.entries(env)) {
            args.push("-e", `${k}=${v}`);
        }
        child = spawn("docker", [...args, CHAT_IMAGE], { stdio: ["ignore", "pipe", "pipe"] });
        cleanups.push(() => {
            try {
                execFileSync("docker", ["rm", "-f", name], { stdio: "ignore" });
            } catch {}
        });
    }
    cleanups.push(() => child.kill("SIGTERM"));
    let output = "";
    await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`chat_server not listening:\n${output}`)), 30000);
        const seen = (chunk) => {
            output += chunk;
            if (output.includes('"msg":"listening"')) {
                clearTimeout(timer);
                resolve();
            }
        };
        child.stdout.on("data", seen);
        child.stderr.on("data", seen);
        child.on("exit", (code) => reject(new Error(`chat_server exited ${code}:\n${output}`)));
    });
    log("chat_server listening on", PORT, CHAT_BIN ? "(binary)" : `(${CHAT_IMAGE})`);
    return () => output;
}

// --- a chat socket ---------------------------------------------------------------------------

export class Socket {
    constructor(user) {
        this.user = user;
        this.frames = [];
        this.waiters = [];
        this.ws = new WebSocket(`ws://127.0.0.1:${PORT}/rt`, {
            headers: { Authorization: `Bearer ${mint(user)}` },
        });
        this.ws.onmessage = (e) => {
            const frame = JSON.parse(e.data);
            this.frames.push(frame);
            this.onFrame?.(frame);
            for (const w of [...this.waiters]) {
                if (w.test(frame)) {
                    this.waiters.splice(this.waiters.indexOf(w), 1);
                    w.resolve(frame);
                }
            }
        };
    }

    open() {
        return new Promise((resolve, reject) => {
            this.ws.onopen = resolve;
            this.ws.onerror = (e) => reject(new Error(`${this.user}: ${e.message ?? "socket error"}`));
        });
    }

    wait(test, what, ms = 10000) {
        const found = this.frames.find((f) => test(f));
        if (found) {
            return Promise.resolve(found);
        }
        return new Promise((resolve, reject) => {
            const w = { test, resolve };
            this.waiters.push(w);
            setTimeout(() => reject(new Error(`${this.user}: no ${what} in ${ms} ms`)), ms);
        });
    }

    async join(room) {
        this.ws.send(JSON.stringify({ type: "join", room }));
        await this.wait((f) => f.type === "joined" && f.room === room, `joined ${room}`);
    }

    send(room, id, body) {
        this.ws.send(JSON.stringify({ type: "send", room, id, body }));
    }

    async sendAndWait(room, bytes) {
        const id = randomUUID();
        this.send(room, id, toBase64url(bytes));
        const answer = await this.wait((f) => f.id === id && (f.type === "sent" || f.type === "error"), `sent ${id}`);
        assert.equal(answer.type, "sent", JSON.stringify(answer));
        return { id, seq: answer.seq };
    }

    // The room's message frames from `after` on, waiting for the one that satisfies `test`.
    message(room, test, what) {
        return this.wait((f) => f.type === "message" && f.room === room && test(f), what);
    }
}

// --- the native device: ffi_peer over a pipe -------------------------------------------------

export class NativeDevice {
    constructor(identity) {
        this.identity = identity;
        this.child = spawn(PEER, [], { stdio: ["pipe", "pipe", "inherit"] });
        this.child.on("error", (e) => {
            for (const waiter of this.queue.splice(0)) {
                waiter(`err ${e.message}`);
            }
        });
        cleanups.push(() => this.child.kill());
        this.lines = createInterface({ input: this.child.stdout });
        this.queue = [];
        this.lines.on("line", (line) => this.queue.shift()?.(line));
    }

    async call(...words) {
        const line = await new Promise((resolve) => {
            this.queue.push(resolve);
            this.child.stdin.write(words.join(" ") + "\n");
        });
        const [status, ...rest] = line.split(" ");
        if (status !== "ok") {
            throw new Error(`ffi_peer ${words[0]}: ${line}`);
        }
        return rest;
    }

    static async create(identity) {
        const d = new NativeDevice(identity);
        await d.call("new", identity);
        return d;
    }

    async keyPackage() {
        return Buffer.from((await this.call("kp"))[0], "hex");
    }

    async process(bytes) {
        const [kind, plaintext] = await this.call("process", Buffer.from(bytes).toString("hex"));
        return { kind, plaintext: Buffer.from(plaintext ?? "", "hex").toString() };
    }

    async encrypt(message) {
        return Buffer.from((await this.call("encrypt", Buffer.from(message).toString("hex")))[0], "hex");
    }

    async number(what) {
        return Number((await this.call(what))[0]);
    }
}

// Runs `main`, then every cleanup; exits non-zero if `main` threw.
export async function run(main) {
    let failed = false;
    try {
        await main();
    } catch (e) {
        failed = true;
        console.error("[interop] FAILED:", e);
    } finally {
        for (const c of cleanups.reverse()) {
            try {
                c();
            } catch (e) {
                console.error("[interop] cleanup:", e.message);
            }
        }
    }
    process.exit(failed ? 1 : 0);
}

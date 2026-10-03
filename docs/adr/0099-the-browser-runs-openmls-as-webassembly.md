# 0099. The browser runs the bridge's OpenMLS as WebAssembly, built in a pinned image

Status: Accepted
Date: 2026-10-04

## Context

ADR-0016 put MLS in the client, as OpenMLS compiled to WebAssembly for the browser, and ADR-0044
put the same library behind a C API for the native harnesses. Only the native side existed. A
browser client must interoperate with it byte for byte: the chat server carries each MLS message
as an opaque body and never translates. There is no key directory API on the chat server yet, so
key packages and welcomes must travel the same way application messages do. The people who use
the browser build should not need a Rust toolchain, and every input to it must be pinned (hard
boundary 6).

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| OpenMLS 0.9.0 with `openmls_rust_crypto`, compiled to `wasm32-unknown-unknown` with wasm-bindgen | The bridge's library, version and provider: one implementation on both sides | Accepted |
| A different MLS implementation in TypeScript (ts-mls) | No WebAssembly, smaller download | Rejected: a second implementation to keep in step, and interop would rest on two readings of RFC 9420 |
| `openmls_libcrux_crypto` on wasm | Formally verified, faster | Rejected for now, as in ADR-0044: the bridge uses RustCrypto, and the two sides should run the same code |
| WebCrypto for the primitives | The browser's own, constant-time code | Rejected: OpenMLS has no WebCrypto provider, and its crypto calls are synchronous where WebCrypto's are not; a provider of our own would be new crypto glue to audit |
| Build dist/ on each machine with a local Rust toolchain | No committed binaries | Rejected: the demo machine has only Docker; committing dist/ with recorded hashes, rebuilt reproducibly in a pinned image, lets anyone check it |

## Decision

- `clients/web-mls` is a crate exposing a small JavaScript API through wasm-bindgen: `MlsClient`
  (new identity, `keyPackage`, `createGroup`, `joinGroup`, `loadGroup`, `exportState`,
  `importState`), `MlsGroup` (`add`, `remove`, `mergePendingCommit`, `clearPendingCommit`,
  `encrypt`, `process`, epoch and members) and `inspect`, which says what a body is without
  keys. Every choice that reaches the wire is the bridge's: ciphersuite 1
  (`MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519`), basic credentials carrying the device id,
  the ratchet tree in the welcome, OpenMLS's default wire format policy, and every message
  serialised as an MLSMessage. Failures throw an `Error` whose message is the bridge's status
  name.
- Pins: `openmls =0.9.0` (with its `js` feature: `web-time` for clocks), `openmls_rust_crypto`,
  `openmls_basic_credential` and `openmls_traits =0.6.0`, the bridge's exact versions; its
  Cargo.lock is the bridge's with only the crates the targets differ by added or dropped, no
  shared crate at another version. Both getrandom majors in the graph (0.2 under `rand_core`
  0.6, 0.4 under OpenMLS) are told to use `crypto.getRandomValues` (`js`, `wasm_js`).
  `wasm-bindgen =0.2.129`, the version the lock already had.
- `build.sh` builds in `rust:1.94.1-slim-bookworm`, pinned by digest, as `linux/amd64`
  everywhere: rustc 1.94.1 as the bridge (ADR-0044), `cargo build --locked`, and the
  wasm-bindgen 0.2.129 release binary checked against its SHA-256. `dist/` (the `.wasm`, the
  `--target web` glue, its typings and `mls-room.js`) is committed, with `dist/SHA256SUMS`;
  `build.sh --check` rebuilds and fails if a byte differs.
- The room is the delivery service. Each body in an encrypted room is one MLSMessage; its wire
  format says whether it is a key package (a device asking to be added), a welcome, or a group
  message, and the group id is the room id's text. The member at the group's first leaf adds an
  announced key package. A member sends its commit, merges it when its echo arrives with no other
  commit for that epoch ahead of it in seq order, and only then sends the welcome.
  `mls-room.js` implements this for pages, so the browser holds no protocol logic of its own.
- State export is the provider's whole store (signature key, unused key packages' private keys,
  every group) with the identity, as one byte string for IndexedDB.
- `interop/run.sh` is the check: the WebAssembly build and the bridge's own code (a C program
  linking `libmls_ffi_bridge.a`) form groups both ways through a real chat_server on a scratch
  database, read each other's messages, follow each other's commits and check the stored bodies
  are the bytes sent and the bridge's framing (plaintext + 166 bytes). `--browser` adds
  `example.html` in two Chromium contexts, with the native device in the same room and a reload
  restored from IndexedDB.

## Consequences

- The `.wasm` is about 2 MB; it loads once per page and is cached.
- Committed binaries must be rebuilt when the crate or its lock changes; `build.sh --check`
  says whether they match the source. Nothing in CI runs it yet.
- Whoever holds exported state is that device. Pages keep it in IndexedDB unencrypted; a
  passphrase or a non-extractable WebCrypto key wrapping it is future work.
- Announcing key packages in the room shows the room's members which devices want to join,
  which the server already knows. When a key directory is served (ADR-0038), key packages move
  there and the room keeps commits, welcomes and application messages.
- A device that joins reads nothing from before its welcome, as ADR-0016 says. A page that
  reconnects reads live messages only; resuming with `after` and applying commits past a gap
  (docs/integration/chat.md) is left to the product client.
- Reopen with ADR-0044: if the bridge moves to libcrux or another OpenMLS version, this crate
  moves with it.

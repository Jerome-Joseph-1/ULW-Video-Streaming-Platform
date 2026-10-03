# End-to-end encryption

> **Draft: changes until milestone M22 merges.** Nothing on `main` serves key directories yet.
> The model is below; the [browser client](#browser-client) carries key packages and welcomes as
> room messages until a directory is served.

Private 1:1 and small-group chat is end-to-end encrypted with MLS (ADR-0016); a 1:1 chat is an
MLS group of two. All keys are generated and held by client devices; the server never holds or
sees them. The server keeps a directory of devices and their single-use KeyPackages, hands one
out per fetch (and signals the owner to replenish when none are left), and carries MLS
handshake and application messages as opaque chat bodies. The room's total order (`seq`) is
what orders Commits: members apply the first valid Commit for an epoch in `seq` order and
discard the rest. Multi-device key management is a known gap.

## Browser client

<!-- clients/web-mls/src/core.rs, clients/web-mls/src/wasm.rs, clients/web-mls/js/mls-room.js, clients/web-mls/build.sh, clients/web-mls/interop/, docs/adr/0099-the-browser-runs-openmls-as-webassembly.md -->

`clients/web-mls` is OpenMLS 0.9.0 compiled to WebAssembly: the library, version, ciphersuite
and message forms of the FFI bridge the native harnesses use (ADR-0044, ADR-0099), so browser
and native devices share groups. Its build is committed in `clients/web-mls/dist/`; a page needs
nothing else.

| File in `dist/` | What it is |
|---|---|
| `web_mls_bg.wasm`, `web_mls.js` | The module and its ES module glue (`wasm-bindgen --target web`) |
| `web_mls.d.ts` | Typings for the API below |
| `mls-room.js` | The room convention below, over the chat socket the page already has, and IndexedDB helpers |
| `SHA256SUMS` | The SHA-256 of each file, as `build.sh` made it |

Rebuild with `clients/web-mls/build.sh` (Docker only: the toolchain image is pinned by digest,
crates by Cargo.lock, wasm-bindgen by its release's SHA-256), and check the committed files with
`build.sh --check`, which rebuilds and compares byte for byte.

### API

Bytes are `Uint8Array`s. A failure throws an `Error` whose `message` is one of `malformed`,
`rejected` (bad signature, wrong group or epoch, a welcome for another device),
`not_a_member`, `inactive` (this device was removed), `invalid_argument` or `internal`.

| Call | Does |
|---|---|
| `await loadMls()` | Loads the module (from `web_mls_bg.wasm` beside `mls-room.js`, or a URL or bytes given) |
| `new MlsClient(identity)` | A new device; `identity` (1 to 64 bytes) is its device id, in its basic credential |
| `client.keyPackage()` | A single-use KeyPackage, as an MLSMessage, to post to the room |
| `client.createGroup(groupId)` | A group at epoch 0 with this device alone in it. The group id is the room id's text |
| `client.joinGroup(welcome)` | Joins from a Welcome; `rejected` if it is for other devices |
| `client.loadGroup(groupId)` | A group this device is in, from its state |
| `client.exportState()`, `MlsClient.importState(bytes)` | The whole device (signature key, unused key packages, every group) as bytes for IndexedDB, and back. Secret |
| `group.add([keyPackage, ...])` | `{commit, welcome}`; the commit stays pending |
| `group.remove(identity)` | A commit removing that member; pending |
| `group.mergePendingCommit()`, `group.clearPendingCommit()` | The room sequenced this device's commit first for its epoch, or another one won |
| `group.encrypt(plaintext)` | An application message (PrivateMessage) |
| `group.process(message)` | Another member's message: `{kind, plaintext, sender}`, `kind` being `application`, `commit` (merged) or `proposal` |
| `group.epoch`, `group.members()`, `group.memberCount`, `group.hasPendingCommit` | The group as this device sees it |
| `inspect(bytes)` | Without keys: `{wireFormat, groupId, epoch, contentType, identity}`; a key package's signature and ciphersuite are checked |

### Messages in the room

There is no key directory on the chat server yet, so everything an MLS group needs travels as
room messages. Each `body` (base64url, as for any body, [chat.md](chat.md)) is exactly one
MLSMessage (RFC 9420, section 6), the bytes the FFI bridge makes and stores:

| Wire format | Meaning | Who acts |
|---|---|---|
| `mls_key_package` (5) | A device asking to be added | The member at the group's first leaf commits an add |
| `mls_private_message` (2) | A commit or an application message for the group; its clear header names the group, the epoch and which of the two it is. OpenMLS's default policy, which the bridge keeps, encrypts handshake messages too | Every other member, in seq order |
| `mls_welcome` (3) | Sent after the commit that added someone was accepted | A device not yet in the group tries to join; others ignore it |
| `mls_public_message` (1) | Not sent by either client; processed like a private one if a member did | |

- The suite is `MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519` (1) on every key package and
  group; a key package for another suite is rejected.
- Commits: for each epoch the first commit in seq order wins; later ones for that epoch are
  discarded. A member sends its commit and merges it when its own echo arrives with no other
  commit for that epoch before it (else clears it); only then does it send the welcome.
- A member never processes its own messages: it knows them by the `id` it sent them with.
- A device that has posted its key package but not yet joined keeps the group's messages that
  arrive before the welcome and processes those of the epoch it joined at.

`mls-room.js` implements this: give `MlsRoom` the client, the room id, a `send(id, body)` that
posts a `send` command, and every `message` frame of the room through `receive(frame)`.

```js
import { loadMls, MlsClient, MlsRoom, utf8, loadState, saveState } from "./dist/mls-room.js";

await loadMls();
const saved = await loadState("alice-laptop");
const client = saved ? MlsClient.importState(saved) : new MlsClient(utf8("alice-laptop"));
const room = new MlsRoom({
  client, room: roomId,
  send: (id, body) => ws.send(JSON.stringify({ type: "send", room: roomId, id, body })),
  onMessage: ({ sender, text }) => show(sender, text),
  onState: (state) => saveState("alice-laptop", state),
});
ws.onmessage = (e) => { const f = JSON.parse(e.data); if (f.type === "message") room.receive(f); };
room.create();          // the first device; every other one calls room.announce()
room.sendText("hello"); // once room.joined
```

`clients/web-mls/example.html` is a page doing the above. Serve `clients/web-mls/` from an
origin listed in the chat server's `ULW_ALLOWED_ORIGINS` and open it in two browser contexts
(a normal and a private window: each needs its own `auth_token` cookie, which the page sets
from the token field); one device starts the group, the other asks to join.

### Interop check

`clients/web-mls/interop/run.sh` builds the bridge and a small C client on it, starts a
chat_server (a binary, or the published image) on a scratch database, and checks in both
directions that the browser build and the bridge form groups, read each other, follow each
other's commits, and that the room stores exactly the bytes each made (an application message is
its plaintext and 166 bytes for a 36-byte group id, as chat_e2ee_test checks for the bridge).
With `--browser` it also drives `example.html` in two Chromium contexts with a native device in
the room, and reloads one page to restore its device from IndexedDB.

Known gaps: exported state is kept in IndexedDB unencrypted; a reconnecting page reads live
messages only (resume with `after` and gap filling, [chat.md](chat.md), are the product
client's); multi-device as above.

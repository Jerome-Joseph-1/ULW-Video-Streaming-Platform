# End-to-end encryption

> **Draft: changes until milestone M22 merges.** Nothing on `main` serves key directories yet.
> The model is below; the [browser client](#browser-client) carries key packages and welcomes as
> room messages until a directory is served.

Private 1:1 and small-group chat is end-to-end encrypted with MLS (ADR-0016); a 1:1 chat is an
MLS group of two. All private keys are generated and held by client devices; the server never
holds or sees them, nor any message key or plaintext. It does carry what decides membership (key
packages, and with the browser client below also welcomes and commits), so without an
authentication service it can still influence who is in a group; see
[Who joins, and what the server can do](#who-joins-and-what-the-server-can-do). The server keeps a directory of devices and their single-use KeyPackages, hands one
out per fetch (and signals the owner to replenish when none are left), and carries MLS
handshake and application messages as opaque chat bodies. The room's total order (`seq`) is
what orders Commits: members apply the first valid Commit for an epoch in `seq` order and
discard the rest. Multi-device key management is a known gap.

Who is in a chat is the server's member list (ADR-0054), changed by its users with the commands
of [chat.md](chat.md#changing-member-lists) (ADR-0096); the MLS group follows it by commits the
members' devices make. A device that adds members commits the Add and sends each new member's
Welcome as room messages; when a `member` frame tells of a removal or a leave, a remaining
member's device commits the Remove. The server cuts a removed member off at once, before any
commit, and never reads one.

## Browser client

<!-- clients/web-mls/src/core.rs, clients/web-mls/src/wasm.rs, clients/web-mls/js/mls-room.js, clients/web-mls/build.sh, clients/web-mls/interop/, docs/adr/0098-the-browser-runs-openmls-as-webassembly.md -->

`clients/web-mls` is OpenMLS 0.9.0 compiled to WebAssembly: the library, version, ciphersuite
and message forms of the FFI bridge the native harnesses use (ADR-0044, ADR-0098), so browser
and native devices share groups. Its build is committed in `clients/web-mls/dist/`; a page needs
nothing else.

| File in `dist/` | What it is |
|---|---|
| `web_mls_bg.wasm`, `web_mls.js` | The module and its ES module glue (`wasm-bindgen --target web`) |
| `web_mls.d.ts` | Typings for the API below |
| `mls-room.js` | The room convention below, over the chat socket the page already has, and IndexedDB helpers |
| `SHA256SUMS` | The SHA-256 of each file, as `build.sh` made it |

Rebuild with `clients/web-mls/build.sh` (Docker only: the toolchain image is pinned by digest,
the wasm32 standard library and wasm-bindgen by SHA-256, crates by Cargo.lock), and check the
committed files with `build.sh --check`, which rebuilds and compares byte for byte; CI runs it.

### API

Bytes are `Uint8Array`s. A failure throws an `Error` whose `message` is one of `malformed`,
`rejected` (bad signature, wrong group or epoch, a welcome for another device, a proposal, an
add not made by the group's first member), `not_a_member`, `inactive` (this device was
removed), `invalid_argument` or `internal`.

| Call | Does |
|---|---|
| `await loadMls()` | Loads the module (from `web_mls_bg.wasm` beside `mls-room.js`, or a URL or bytes given) |
| `new MlsClient(identity)` | A new device; `identity` (1 to 64 bytes), `<user id>/<device>` by convention, is in its basic credential |
| `client.fingerprint` | SHA-256 of the device's signature key, hex: what people compare out of band |
| `client.keyPackage()` | A single-use KeyPackage, as an MLSMessage, to post to the room |
| `client.createGroup(groupId)` | A group at epoch 0 with this device alone in it. The group id is the room id's text |
| `client.joinGroup(welcome, expectedGroupId?)` | Joins from a Welcome; `rejected` if it is for other devices, for another group than `expectedGroupId`, or for a group this device is in. A refused welcome leaves nothing behind |
| `client.loadGroup(groupId)` | The group, from this client's state. Every handle to a group shares one state |
| `client.exportState()`, `MlsClient.importState(bytes)` | The whole device (signature key, unused key packages, every group, `appData`) as bytes for IndexedDB, and back. Secret |
| `client.appData` | Bytes the application keeps in the state (`mls-room.js` keeps its outbox there) |
| `group.add([keyPackage, ...])` | `{commit, welcome}`; the commit stays pending |
| `group.remove(identity)` | A commit removing that member; pending |
| `group.mergePendingCommit()`, `group.clearPendingCommit()` | The room sequenced this device's commit first for its epoch, or another one won |
| `group.encrypt(plaintext)` | An application message (PrivateMessage) |
| `group.process(message)` | Another member's message: `{kind, plaintext, sender, senderLeaf, selfRemoved}`, `kind` being `application` or `commit` (merged). Refuses proposals, and commits adding members from anyone but the first member |
| `group.epoch`, `group.members()`, `group.memberCount`, `group.hasPendingCommit`, `group.active` | The group as this device sees it; `members()` is `[{leaf, identity, fingerprint}]` |
| `inspect(bytes)` | Without keys: `{wireFormat, groupId, epoch, contentType, identity, fingerprint, keyPackageRef}`; a key package's signature and ciphersuite are checked |

Groups keep the message secrets of the last four epochs, so an application message encrypted
just before a commit and sequenced after it still decrypts. Nothing on the wire depends on it.

### Messages in the room

There is no key directory on the chat server yet, so everything an MLS group needs travels as
room messages. Each `body` (base64url, as for any body, [chat.md](chat.md)) is exactly one
MLSMessage (RFC 9420, section 6), the bytes the FFI bridge makes and stores:

| Wire format | Meaning | Who acts |
|---|---|---|
| `mls_key_package` (5) | A device asking to be added | The member at the group's first leaf, if its user approves and the credential's user part is the chat user who posted it |
| `mls_private_message` (2) | A commit or an application message for the group; its clear header names the group, the epoch and which of the two it is. OpenMLS's default policy, which the bridge keeps, encrypts handshake messages too | Every other member, in seq order |
| `mls_welcome` (3) | Sent after the commit that added someone was accepted | A device that announced itself and is not yet in the group, into this room's group only; others ignore it |
| `mls_public_message` (1) | Not sent by either client; processed like a private one if a member did | |

- The suite is `MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519` (1) on every key package and
  group; a key package for another suite is rejected.
- Commits: for each epoch the first commit in seq order wins; later ones for that epoch are
  discarded. A commit adding members counts only from the member at the first leaf; one from
  anyone else is refused and not applied. Standalone proposals are dropped. A member sends its
  commit and merges it when its own echo arrives with no other commit for that epoch before it
  (else clears it); only then does it send the welcome. This ordering by the room's seq is
  interim: the directory's epoch claim (ADR-0038) is not used on this path yet.
- A member never processes its own messages: it knows them by the `id` it sent them with and
  its own user as `sender`.
- A device that has announced itself but not yet joined keeps up to 256 of the group's
  messages that arrive before the welcome, and processes those of the epoch it joined at.

### Who joins, and what the server can do

There is no authentication service: a credential is a name its device chose. What the browser
client checks is that a key package's user part (`alice` in `alice/laptop`) is the chat user
who posted it, which the server vouches for; the rest is up to people. `approveKeyPackage`
decides whether to add a device and says no unless the page supplies it, and the page should
ask its user, showing the device's fingerprint, which the device's own page also shows
(`client.fingerprint`), so the two can be compared out of band. Members and their fingerprints
come with the `joined`, `added` and `commit` events.

The server cannot read messages, but it carries every key package, welcome and commit, so it
can still influence membership: post a key package for a user it controls, withhold or reorder
messages, keep a device from ever being added or from hearing a commit. Fingerprints are how
people find out. Only the first leaf adds, so while that device is offline nobody joins, and if
it is gone for good the group has to be started again.

### Using it from a page

`mls-room.js` implements the convention: give `MlsRoom` the client, the room id, the chat user,
a `send(id, body)` that posts a `send` command, an `approveKeyPackage`, and an `onState` that
saves (it is awaited, and nothing is posted until it resolves); pass every `message` frame of
the room, history first, to `receive(frame)`.

```js
import { loadMls, MlsClient, MlsRoom, utf8, loadState, saveState } from "./dist/mls-room.js";

await loadMls();
const device = `${user}/laptop`;
const saved = await loadState(device); // throws device_in_use if another tab has it
const client = saved ? MlsClient.importState(saved) : new MlsClient(utf8(device));
const room = new MlsRoom({
  client, room: roomId, user,
  send: (id, body) => ws.send(JSON.stringify({ type: "send", room: roomId, id, body })),
  approveKeyPackage: ({ identity, chatSender, fingerprint }) => askThePerson(identity, fingerprint),
  onMessage: ({ sender, text }) => show(sender, text),
  onEvent: (e) => showEvent(e), // created, adding, added, lost, joined, commit, removed, denied, error
  onState: (state) => saveState(device, state),
});
ws.onmessage = (e) => {
  const f = JSON.parse(e.data);
  if (f.type === "message") room.receive(f);
  if (f.type === "error" && f.id) setTimeout(() => room.sendFailed(f.id, f.reason), f.retry_after_ms ?? 500);
};
// once history is in:
await room.resume();         // posts what a reload left outstanding, under the same ids
await room.create();         // the first device; every other one: await room.announce()
const id = await room.sendText("hello"); // once room.joined
```

Everything the room posts goes through an outbox kept in the device's state: an entry leaves
it when its echo comes back, `resume()` posts what is left under the same ids (chat sequences a
resent id once), and `sendFailed(id, reason)` posts one again, or drops it on a final refusal
(`conflict`, `not_member`, ...). A commit is never lost to a reload or a failed send.
`loadState` holds the device for its tab (Web Locks), and `saveState` refuses a device the tab
does not hold, so two tabs never write one device's state.

`clients/web-mls/example.html` is a page doing the above. Serve `clients/web-mls/` from an
origin listed in the chat server's `ULW_ALLOWED_ORIGINS` and open it in two browser contexts
(a normal and a private window: each needs its own `auth_token` cookie, which the page sets
from the token field); one device starts the group, the other asks to join, and the first is
asked to approve it, with its fingerprint.

### Interop check

`clients/web-mls/interop/run.sh` runs `room.test.mjs` (the convention against an in-memory
room: approval, forged credentials, foreign welcomes, save before post, a lost commit across a
reload), then builds the bridge and a small C client on it, starts a chat_server (a binary, or
the published image, by digest) on a scratch database, and checks in both directions that the
browser build and the bridge form groups, read each other, follow each other's commits, and
that the room stores exactly the bytes each made (an application message is its plaintext and
166 bytes for a 36-byte group id, as chat_e2ee_test checks for the bridge). With `--browser` it
also drives `example.html` in two Chromium contexts with a native device in the room, approving
by the fingerprint shown, refusing a second tab of a device, and reloading one page to restore
its device from IndexedDB.

Known gaps: exported state is kept in IndexedDB unencrypted; a reconnecting page reads what
the caller feeds it (resume with `after` and gap filling, [chat.md](chat.md), are the product
client's); members cannot be removed from a page; multi-device as above.

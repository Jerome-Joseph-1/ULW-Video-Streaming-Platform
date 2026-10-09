# End-to-end encryption

> **Stable.** The key directory's commands and answers below, and the room convention of the
> [browser client](#browser-client), are kept under the compatibility rules in
> [versioning.md](versioning.md) (milestone M22, ADR-0101).

Private 1:1 and small-group chat is end-to-end encrypted with MLS (ADR-0016); a 1:1 chat is an
MLS group of two. All private keys are generated and held by client devices; the server never
holds or sees them, nor any message key or plaintext. It does carry what decides membership (key
packages, and with the browser client below also welcomes and commits), so without an
authentication service it can still influence who is in a group; see
[Who joins, and what the server can do](#who-joins-and-what-the-server-can-do). The server keeps a
[key directory](#key-directory) of each user's devices and their KeyPackages: single-use ones,
each handed out once, and one last-resort package per device for when those run out; it tells a
device to publish more, and carries MLS handshake and application messages as opaque chat
bodies. The room's total order (`seq`) is what orders Commits: members apply the first valid
Commit for an epoch in `seq` order and discard the rest.

Every device is its own MLS member ([Multi-device](#multi-device)): a person with a phone and a
laptop is two leaves of each group, and no key ever leaves the device that made it.

Who is in a chat is the server's member list (ADR-0054), changed by its users with the commands
of [chat.md](chat.md#changing-member-lists) (ADR-0096); the MLS group follows it by commits the
members' devices make. The device that adds members lists every listed user's devices in the
directory, claims a key package of each device the group lacks, commits the Add and sends the
Welcome as a room message; when a `member` frame tells of a removal or a leave, or a user retires
a device, it commits the Remove. The server cuts a removed member off at once, before any
commit, and never reads one.

## Key directory

<!-- apps/chat/src/key_directory.hpp, apps/chat/src/key_directory.cpp, apps/chat/src/envelope.hpp, apps/chat/src/envelope.cpp, apps/chat/src/session.cpp (command), core/include/core/ports/e2ee.hpp, infra/postgres/src/e2ee_directory.cpp, migrations/0004_e2ee_directory.sql, migrations/0018_e2ee_last_resort.sql, docs/adr/0038-single-use-key-packages-in-postgres.md, docs/adr/0101-the-key-directory-is-served-and-every-device-is-a-member.md -->

The directory is served on the chat WebSocket ([chat.md](chat.md#connecting)): the same socket,
token and envelope as everything else, with the commands below. None needs a room joined. Each
takes an optional `id` (1 to 64 of `A-Z a-z 0-9 _ -`) that its answer, or its `error`, repeats,
so a client with several requests out can tell the answers apart. Key packages are MLSMessages
(RFC 9420, section 6) as the device serialised them, in base64url without padding, 1 to 8192
bytes each; the server never parses one.

**Devices.** A device id is a UUID the device mints once and keeps with its state (lowercase,
canonical). Its MLS credential's identity is `<user id>/<device id>`, and that is what the
device adding it checks a claimed package against. A user has at most 16 live devices.
Registering is idempotent: register on every connect. A retired device is gone for good: its
packages are deleted, and its id is refused if it comes back.

Client to server:

| `type` | Fields | Meaning |
|---|---|---|
| `register_device` | `device`; `id` optional | This user's device, registered if it is not yet. Answered `device_registered` |
| `publish_key_packages` | `device`; `key_packages` (0 to 100), `last_resort` (one), at least one of the two; `id` optional | Store single-use packages, all or none, and replace the device's last-resort package. Only the device's own user may. Answered `key_packages_published` |
| `retire_device` | `device`; `id` optional | Retire the device for good. Answered `device_retired` |
| `devices` | `user`; `id` optional | The user's live devices: your own, or those of someone you share a direct or group chat with |
| `claim_key_packages` | `user`; `devices` (1 to 16 device ids, each once) optional; `id` optional | One package of each named device of `user`, or of each live one without `devices`, for adding them to a group. The same rule as `devices` on whose |

Server to client:

| `type` | Fields | Meaning |
|---|---|---|
| `device_registered`, `key_packages_published` | `device`, `key_packages` (how many single-use packages it holds), `last_resort` (`none`, `fresh`, or `used`: handed out since it was published), `id` | The device's supply after the command. Publish more when it is low, and a new last resort when it is not `fresh` |
| `device_retired` | `device`, `id` | |
| `devices` | `user`, `devices` (`[{"device"}]`, in byte order; for your own also `key_packages` and `last_resort` each), `id` | |
| `key_packages` | `user`, `key_packages` (`[{"device","key_package","last_resort"}]`), `exhausted`, `gone`, `unavailable` (device ids), `id` | What each device gave: a package (`last_resort` `true` when it is the device's last resort), nothing left (`exhausted`), not, or no longer, a live device of that user (`gone`), or the database did not answer for it (`unavailable`: claim it again) |
| `replenish` | `device` | Unasked, to the connections on the same node that registered the device: a claim left it with a few single-use packages, none, or on its last resort. Register again to read the supply, and publish |

```json
{"type":"register_device","device":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0e","id":"r1"}
{"type":"device_registered","device":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0e","key_packages":0,"last_resort":"none","id":"r1"}
{"type":"publish_key_packages","device":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0e","key_packages":["AAEABQAB..."],"last_resort":"AAEABQAB..."}
{"type":"key_packages_published","device":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0e","key_packages":1,"last_resort":"fresh"}
{"type":"claim_key_packages","user":"user-42","id":"c7"}
{"type":"key_packages","user":"user-42","key_packages":[{"device":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0e","key_package":"AAEABQAB...","last_resort":false}],"exhausted":[],"gone":[],"unavailable":[],"id":"c7"}
```

**Single use.** A claim takes each device's package with one statement that deletes the
package it returns, so of any number of claims racing for a device's last package, through any
nodes, exactly one gets it; the others get the device's last-resort package, or `exhausted`
without one. Packages are handed out oldest first. A claim spends its packages whether or not
they are used: a client that drops the answer, or whose commit loses its epoch, claims again.

**Last resort** (RFC 9420, section 16.8). Each device keeps one last-resort package, published
beside its single-use ones and replaced by each publish of another. It is handed out only when
the device has no single-use package left, and it is kept, so a device offline for weeks can
still be added to new groups. Every group that took it shares its init key until the device
updates its leaf, so a device replaces it once its status reads `used`.

**Who may ask.** Only a device's own user registers, publishes to or retires it: another user's
device id reads as `unknown_device`, registration included, so ids cannot be probed. Another
user's devices are listed and claimed only by someone who shares a direct or group chat with
them now, as presence is seen (ADR-0096); anyone else is refused `not_shared`, and nothing is
taken. A stream's live chat shares nothing.

**Replenishing.** Keep about 20 single-use packages published and a fresh last resort: register
on every connect and publish what the answer says is missing, again after each `replenish`, and
after taking each welcome (a package was spent). `replenish` reaches only connections on the
node that served the claim, so the check on connect is what a device on another node relies on.

Errors (`error` with `reason`, the request's `id`, and `device` or `user` when the command named
one):

| `reason` | Meaning | Client action |
|---|---|---|
| `malformed`, `bad_id`, `bad_device`, `bad_user`, `bad_body` | Not a command the server can read: a missing or unknown field, an id that is not a request id, a device that is not a canonical lowercase UUID, a package that is not base64url or is empty, a device named twice, more than 100 packages, none at all | Fix the client |
| `too_large` | A package above 8192 bytes | Fix the client |
| `unknown_device` | No such device of yours: never registered, or another user's | Register it (another user's: mint a new device id) |
| `device_retired` | The device was retired | Mint a new device id, with a new MLS client |
| `device_limit` | You have 16 live devices | Retire one first |
| `key_packages_full` | The device would hold more than 100 single-use packages; nothing was stored | Publish fewer |
| `not_shared` | You share no direct or group chat with `user` | Do not retry until you share one |
| `rate_limited` | Past the allowance below; `retry_after_ms` says when | Wait that long |
| `busy` | This connection has 8 directory commands waiting for an answer | Wait for some answers |
| `unavailable` | The database could not be reached | Retry |

Limits, per user across the user's connections on one node: `devices` and `claim_key_packages`
a burst of 120, then 2 a second; `register_device`, `publish_key_packages` and `retire_device` a
burst of 20, then one each 3 s. 8 directory commands waiting per connection. A command frame is
at most 64 KiB, which bounds one publish to what fits in it.

## Multi-device

<!-- clients/web-mls/js/mls-room.js (MlsDirectory, reconcile), docs/adr/0101-the-key-directory-is-served-and-every-device-is-a-member.md -->

Every device of a user is its own leaf in each of the user's groups, with its own signature key
and credential (`<user id>/<device id>`). Nothing is shared between a user's devices, and the
server synchronises no keys: a device reads what is sent after it was added, and nothing from
before (ADR-0016).

The device at the group's first leaf keeps the group in line with the room's member list:

1. For each user on the list, it lists their live devices (`devices`).
2. A device without a leaf: it claims a package for it (`claim_key_packages` naming the devices
   it lacks), checks that the package is a key package whose credential's identity is exactly
   `<user>/<device>` of the device it claimed, asks its person (showing the fingerprint), and
   adds every approved device of every user in one commit, followed by one welcome.
3. A leaf whose user is no longer on the list, or whose device its user retired, is removed, one
   commit each.

It does this when it reads the member list, on each `member` frame, and every few seconds while
it is open, so a device registered since is found on its next look. A device denied is not
asked about again by that device. A new device of a user joins every group that user is in
this way, without any of the user's other devices taking part.

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
| `mls-room.js` | The room convention below and the key directory, over the chat socket the page already has, and IndexedDB helpers |
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
| `new MlsClient(identity)` | A new device; `identity` (1 to 64 bytes), `<user id>/<device id>` with the key directory, is in its basic credential |
| `client.fingerprint` | SHA-256 of the device's signature key, hex: what people compare out of band |
| `client.keyPackage()` | A single-use KeyPackage, as an MLSMessage, to publish to the key directory (or, the old way, to post to the room) |
| `client.lastResortKeyPackage()` | A last-resort KeyPackage, as an MLSMessage, for the directory: welcomes that use it leave its private key in the state, since it may be handed out again |
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
| `inspect(bytes)` | Without keys: `{wireFormat, groupId, epoch, contentType, identity, fingerprint, keyPackageRef, lastResort}`; a key package's signature and ciphersuite are checked |

Groups keep the message secrets of the last four epochs, so an application message encrypted
just before a commit and sequenced after it still decrypts. Nothing on the wire depends on it.

`mls-room.js` speaks the [key directory](#key-directory) through `MlsDirectory`:

| Call | Does |
|---|---|
| `new MlsDirectory({client, device, send, onState, target?, onEvent?})` | The directory for this device: `device` its id (a lowercase UUID), `send(command)` posts one command as JSON on the chat socket, `onState` saves the client's state, awaited before any package it made is published. `target` single-use packages are kept published (20) |
| `directory.receive(frame)` | Every frame of the socket: takes the directory's answers and `replenish`, and returns whether it did |
| `await directory.ensureStock()` | Registers the device and publishes what its supply lacks: on every connect; also run after `replenish` and after taking a welcome |
| `await directory.devices(user)`, `await directory.claim(user, devices?)`, `await directory.retire()` | The commands, answered; a refusal rejects with its `reason` as the message, after waiting out `rate_limited`, `busy` and `unavailable` a few times |
| `inOrder(save)` | Wraps a saver so that the states handed to it are written one at a time, in order: give the same one to the rooms and the directory of a device |

### Messages in the room

Key packages go through the [key directory](#key-directory); everything else an MLS group
needs travels as room messages. Each `body` (base64url, as for any body, [chat.md](chat.md)) is
exactly one MLSMessage (RFC 9420, section 6), the bytes the FFI bridge makes and stores:

| Wire format | Meaning | Who acts |
|---|---|---|
| `mls_key_package` (5) | A device asking to be added the old way, without the directory; still accepted, so clients that do not use the directory keep working | The member at the group's first leaf, if its user approves and the credential's user part is the chat user who posted it |
| `mls_private_message` (2) | A commit or an application message for the group; its clear header names the group, the epoch and which of the two it is. OpenMLS's default policy, which the bridge keeps, encrypts handshake messages too | Every other member, in seq order |
| `mls_welcome` (3) | Sent after the commit that added someone was accepted | A device that announced itself (published to the directory, or posted a key package) and is not yet in the group, into this room's group only; others ignore it |
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
client checks is that a key package claimed from the directory names exactly the device it was
claimed for (`alice/<device id>`, a device the server lists as alice's), or, for one posted to
the room the old way, that its user part (`alice` in `alice/laptop`) is the chat user who posted
it; the server vouches for both. The rest is up to people. `approveKeyPackage`
decides whether to add a device and says no unless the page supplies it, and the page should
ask its user, showing the device's fingerprint, which the device's own page also shows
(`client.fingerprint`), so the two can be compared out of band. Members and their fingerprints
come with the `joined`, `added` and `commit` events.

The server cannot read messages, but it holds every key package and carries every welcome and
commit, so it can still influence membership: register a device and hand out its package for a
user it controls, withhold a package or a message, reorder messages, keep a device from ever
being added or from hearing a commit. Fingerprints are how
people find out. Only the first leaf adds, so while that device is offline nobody joins, and if
it is gone for good the group has to be started again.

### Using it from a page

`mls-room.js` implements the convention: give `MlsRoom` the client, the room id, the chat user,
a `send(id, body)` that posts a `send` command, an `approveKeyPackage`, an `onState` that saves
(it is awaited, and nothing is posted until it resolves) and the device's `MlsDirectory`; pass
every `message` frame of the room, history first, to `receive(frame)`, and the room's member
list to `reconcile(users)` whenever it is read or a `member` frame comes.

```js
import { loadMls, MlsClient, MlsRoom, MlsDirectory, utf8, loadState, saveState, inOrder } from "./dist/mls-room.js";

await loadMls();
const deviceId = localStorage.getItem("device") ?? crypto.randomUUID(); // minted once, kept
const device = `${user}/${deviceId}`;
const saved = await loadState(device); // throws device_in_use if another tab has it
const client = saved ? MlsClient.importState(saved) : new MlsClient(utf8(device));
const save = inOrder((state) => saveState(device, state));
const directory = new MlsDirectory({ client, device: deviceId, onState: save,
  send: (command) => ws.send(JSON.stringify(command)) });
const room = new MlsRoom({
  client, room: roomId, user, directory,
  send: (id, body) => ws.send(JSON.stringify({ type: "send", room: roomId, id, body })),
  approveKeyPackage: ({ identity, fingerprint }) => askThePerson(identity, fingerprint),
  onMessage: ({ sender, text }) => show(sender, text),
  onEvent: (e) => showEvent(e), // created, adding, added, removing, lost, joined, commit, removed, denied, exhausted, error
  onState: save,
});
ws.onmessage = (e) => {
  const f = JSON.parse(e.data);
  if (directory.receive(f)) return;
  if (f.type === "message") room.receive(f);
  if (f.type === "members" && f.room === roomId) room.reconcile(f.members.map((m) => m.user));
  if (f.type === "member" && f.room === roomId) askForMembers();
  if (f.type === "error" && f.id) setTimeout(() => room.sendFailed(f.id, f.reason), f.retry_after_ms ?? 500);
};
// once history is in:
await directory.ensureStock(); // registers this device and publishes its key packages
await room.resume();           // posts what a reload left outstanding, under the same ids
await room.create();           // the first device; every other one: await room.announce()
askForMembers();               // {"type":"members","room":roomId}, now, on member frames, every few seconds
const id = await room.sendText("hello"); // once room.joined
```

Without a `directory`, `announce()` posts a key package to the room as before, and the first
member's device adds it when it arrives.

Everything the room posts goes through an outbox kept in the device's state: an entry leaves
it when its echo comes back, `resume()` posts what is left under the same ids (chat sequences a
resent id once), and `sendFailed(id, reason)` posts one again, or drops it on a final refusal
(`conflict`, `not_member`, ...). A commit is never lost to a reload or a failed send.
`loadState` holds the device for its tab (Web Locks), and `saveState` refuses a device the tab
does not hold, so two tabs never write one device's state.

`clients/web-mls/example.html` is a page doing the above. Serve `clients/web-mls/` from an
origin listed in the chat server's `ULW_ALLOWED_ORIGINS` and open it in two browser contexts
(a normal and a private window: each needs its own `auth_token` cookie, which the page sets
from the token field); one device starts the group, the other asks to join (it publishes its key
packages to the directory), and the first, which reads the member list every few seconds, finds
the new device there and is asked to approve it, with its fingerprint.

### Interop check

`clients/web-mls/interop/run.sh` runs `room.test.mjs` (the convention against an in-memory
room and directory: approval, forged credentials, foreign welcomes, save before post, a lost
commit across a reload; devices found through the directory, a user's second device, a package
naming another device, removals, the last resort), then builds the bridge and a small C client on it, starts a chat_server (a binary, or
the published image, by digest) on a scratch database, and checks in both directions that the
browser build and the bridge form groups, read each other, follow each other's commits, and
that the room stores exactly the bytes each made (an application message is its plaintext and
166 bytes for a 36-byte group id, as chat_e2ee_test checks for the bridge); in a third room a
browser device adds, from the key directory, its user's second browser device and the bridge's
device in one commit. With `--browser` it
also drives `example.html` in two Chromium contexts with a native device in the room, approving
by the fingerprint shown, refusing a second tab of a device, and reloading one page to restore
its device from IndexedDB.

Known gaps: exported state is kept in IndexedDB unencrypted; a reconnecting page reads what
the caller feeds it (resume with `after` and gap filling, [chat.md](chat.md), are the product
client's); only the device at the first leaf adds and removes, so while it is closed nobody is
added or removed; the private keys of replaced last-resort packages stay in the exported state;
the directory's epoch claims (ADR-0038) are not used, and commits are ordered by the room's seq.

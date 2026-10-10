# 0102. The key directory is served on the chat socket, a device keeps a last-resort package, and every device is a member

Status: Accepted
Date: 2026-10-09

## Context

ADR-0038 built a directory of devices and single-use KeyPackages in Postgres, and rejected
last-resort packages "for now". Nothing served it: the browser client (ADR-0098) posted key
packages to the room it wanted to join, where the device at the group's first leaf added them,
and ADR-0016, ADR-0038 and ADR-0044 all left multi-device key management open: one person's
devices were unrelated members, and nothing told a group that a user had a new device. M22 is
the milestone that takes end-to-end encryption out of draft, and it needs both. What does not
change: the server holds no private key, no group secret and no plaintext, never parses a
package, and the chat service and the room router do not change for it (ADR-0043).

## Options

| Question | Option | Verdict |
|---|---|---|
| Where clients reach the directory | New commands on the chat WebSocket, dispatched by the session to a service of their own, as ADR-0043 planned | Accepted: the socket is already authenticated, rate limited and open; no second endpoint for browsers to hold |
| | HTTP endpoints on the gateway | Rejected: a second authenticated surface, CORS and cookies for a browser, and the gateway does not know chat's member lists |
| | Through the chat service and the room plane | Rejected: the service and the router must not change for E2EE (ADR-0043, `tools/e2ee_diagnostic_check.sh`), and a package is nobody's room message |
| Who may read a user's devices and claim their packages | Anyone signed in | Rejected: drains anyone's supply, and lists who has how many devices |
| | Their own user, and users who share a direct or group chat with them now (ADR-0096's presence rule) | Accepted: the people who could invite them; the store answers it by index |
| | Only the room's members, with a room named per claim | Rejected: inviting someone happens before they are in the room's MLS group, and the member list (ADR-0096) already says who shares a chat |
| A device out of single-use packages | Exhausted until it comes back online (ADR-0038) | Rejected now: a device offline for a week cannot be added to anything new |
| | A last-resort package per device (RFC 9420 section 16.8), handed out only when no single-use one is left, kept, and marked used | Accepted: invitations do not fail for want of packages; reuse is bounded to the interval until the device publishes another, and it is told to |
| | Unlimited reuse of any package | Rejected: what single use guards against |
| Multi-device | One MLS member per user, with keys synchronised between that user's devices | Rejected: private keys would have to move between devices, through the server or beside it, and the server would hold or carry them |
| | Every device its own leaf, `<user>/<device id>` in its basic credential, found through the directory; the group's adder adds every live device of every member and removes devices retired or users no longer listed | Accepted: no key ever leaves its device, and the member list stays the one authority on who is in a chat |
| How the adder learns of a new device | A server push to every member's sockets when a device registers | Rejected for now: a notification across nodes to every user who shares a chat, for an event the adder's next look finds anyway |
| | The adder lists the members' devices when it reads the member list, on each `member` frame, and every few seconds while open | Accepted: idempotent, and one listing per member per look |

## Decision

- **Commands** (additive, `docs/integration/e2ee.md`): `register_device` (idempotent; answered
  with the device's supply), `retire_device`, `publish_key_packages` (0 to 100 single-use
  packages and an optional last-resort one, at least one in all), `devices` (a user's live
  devices; the supply only for one's own) and `claim_key_packages` (one package of each named
  device, or of every live one). Each takes an optional `id` that its answer and its error
  repeat. Packages are base64url, 1 to 8192 bytes each, opaque.
- `apps/chat/src/key_directory.cpp` is the service. The session hands it these commands; the chat
  service and the router are untouched. It decides who may ask (a device's packages are written
  only by its own user, which the store checks against the device row; another user's devices
  are read only by someone who shares a direct or group chat with them, else `not_shared`),
  how often (per user and node: listings and claims 120 at once then 2 a second, writes 20 at
  once then one each 3 s, 8 commands in flight per connection), and answers. A claim lists the
  user's live devices, then takes a package of each with ADR-0038's single statement, one device
  at a time per statement, so two claimers never get the same package. The answer names what
  each device gave: a package (marked when it is the last resort), `exhausted`, `gone` (not, or
  no longer, a live device) or `unavailable`.
- **Last resort.** Migration 0018 adds `last_resort_key_packages`, one row per device, replaced
  by each publish of one. The fetch statement hands it out, and keeps it, only when it took no
  single-use package, setting `served_at`; a device's listing shows it `none`, `fresh` or
  `used`. Retiring a device deletes it with the device's other packages.
- **Replenish.** A claim that leaves a device at the low-water mark, empty, or on its last
  resort sends `replenish` to that device's connections on the same node that registered it.
  Other nodes' connections learn it when the device registers again, which clients do on every
  connect; the browser client also tops up after each welcome it takes. Its target is 20
  single-use packages and a fresh last resort.
- **Devices.** A device id is a UUID the client mints once; its credential is
  `<user>/<device id>`. The device at the group's first leaf (ADR-0098's rule) brings the group in
  line with the room's member list: for each listed user, every live device without a leaf is
  claimed, checked (the package must be a key package whose credential is exactly
  `<user>/<device id>` of the device claimed), approved by the page, and added in one commit;
  then a device whose user is not listed, or that its user retired, is removed, one commit at a
  time. A device denied is not asked about again. Devices named the old way (`alice/laptop`) are
  removed only with their user.
- Backwards compatibility (versioning rule 1): everything is additive. A room still accepts a key
  package posted to it, and mls-room.js still adds one, so pages and native clients that do not
  use the directory keep working beside those that do.
- Two Postgres sessions per chat node for the directory (`application_name` `ulw-e2ee`).
- Metrics: `e2ee_*` counters on the asking client's node (operator-contract.md).

## Consequences

- E2EE leaves draft with M22: `docs/integration/e2ee.md` is Stable.
- A device keeps the private key of its last-resort package after welcomes use it, as the RFC
  requires, so every group that took it shares that init key until the device updates its leaf;
  the browser client replaces a used last resort on its next connect or `replenish`, and old
  last-resort private keys stay in its exported state (pruning them is future work).
- A claim spends packages even when its client leaves before the answer, or the commit they were
  claimed for loses its epoch: the devices publish more.
- What the server can still do, as before: hand out a package of a device registered under a
  user it controls, withhold one, or withhold or reorder messages. It cannot hand out a package
  for a device of another user's choosing under a credential the adder checks, since the adder
  binds the credential to the device it claimed. Fingerprints remain how people check devices.
- Liveness stays ADR-0098's: only the first leaf adds and removes. Commits are still ordered by
  the room's seq; the directory's epoch claims (`submit_commit`, ADR-0038) are not served yet.
- A new device reads nothing sent before it was added (ADR-0016), and finding it waits for the
  adder's next look: seconds while the adder's page is open, until it opens otherwise.
- Reopen when the first-leaf rule is lifted, when epoch claims are served, or if a cross-node
  push for new devices or replenishment is needed.

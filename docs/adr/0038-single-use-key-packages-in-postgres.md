# 0038. Single-use key packages in Postgres, with a replenish signal

Status: Accepted
Date: 2026-09-29

## Context

Adding a member to an MLS group (RFC 9420) takes one of the invitee's KeyPackages: a signed,
public bundle holding a fresh HPKE init key whose private half only the invitee's device has.
RFC 9420 section 16.8 requires that a KeyPackage be used once, apart from an optional last-resort
package; reusing one lets two Welcome messages share an init key. Every device is its own member
(ADR-0016), so every package belongs to one device of one user. Clients publish packages ahead of
time, since the invitee is often offline when someone invites them. The server must hand each one
out at most once, even when several people invite the same device at once, must stop handing out
a device's packages once the device is gone, and must tell a device when to publish more. It
never parses a package.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| A table of packages; a fetch is one DELETE of the oldest row that returns it | One statement both reserves and consumes, so no crash leaves a package half-taken; Postgres is already the system of record (ADR-0006) | Accepted |
| SELECT a package, then DELETE it in a second statement | Simpler SQL | Rejected: two fetchers can both read the row before either deletes it |
| A `used` flag, set on fetch, swept later | Keeps an audit trail | Rejected: keeps key material the protocol says is spent, and every reader must remember the flag |
| Packages in Redis, popped with LPOP | Atomic pop, fast | Rejected: a second stateful store for a control-plane rate of one fetch per invitation |
| Last-resort packages, reusable once the rest run out | Invitations never fail for lack of packages | Rejected for now: reuse is exactly what single use guards against; revisit with the client team |

## Decision

- `devices(id, user_id, revoked_at)` and `key_packages(id, device_id, body bytea)` in migration
  0004. A device id is a UUIDv7 the client mints, so registration is idempotent.
- `fetch_key_package` is one statement: it locks the device row FOR SHARE, deletes the device's
  oldest package, picked `FOR UPDATE SKIP LOCKED`, and returns its body. Of any number of
  concurrent fetchers of the last package exactly one gets it; the rest get `Exhausted`.
- `Exhausted` is the replenish signal. A successful fetch also carries `replenish` once the
  device is at or below the low-water mark, a fifth of the cap, so the signal can reach the
  device before it runs dry.
- Bounds: at most 100 packages per device, 1 to 8192 bytes each. A batch is stored whole or not
  at all; publishes of one device serialise on its row lock and count on a snapshot taken after
  the lock, so concurrent publishes never pass the cap.
- `deregister_device` retires the row (`revoked_at`) and deletes its packages in one transaction.
  A retired id never comes back, and fetches of it report `Revoked`.
- `submit_commit(room, user, device, epoch)` claims an epoch transition: the primary key
  `(room_id, epoch)` of `mls_epochs` lets the first claim win and the rest see `StaleEpoch`. The
  commit's bytes travel through the room as an ordinary opaque message, so the room owner
  (ADR-0015) can order commits without the server ever reading one.

## Consequences

- The server stores public key material only and never parses it: a malformed package is the
  inviting client's problem to report, and the directory cannot check lifetimes or ciphersuites.
- An exhausted device cannot be invited until it comes online and publishes again.
- `mls_epochs` gains a row per commit and nothing prunes it yet; the room lifecycle owns that.
- Claims check the committer's device, not its membership in the room: membership lives with
  chat, which calls `submit_commit` from the room owner.
- Multi-device key management stays open (ADR-0016): one person's devices are unrelated members,
  and adding a new device needs a commit by a device already in each group.
- Reopen if invitation failures for lack of packages become common (last-resort packages), or if
  the server has to validate packages.

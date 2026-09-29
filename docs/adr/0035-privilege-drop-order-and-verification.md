# 0035. Dropping root: groups, group, user, then proof

Status: Accepted
Date: 2026-09-29

## Context

Under systemd the units say `User=ulw`, and in the images the process starts unprivileged
already. A binary started by hand or by a supervisor that leaves it root, for example to bind
a low port or to raise a limit, must give root up before it reads a request. Getting the calls
in the wrong order, or checking nothing afterwards, is the classic way to keep a supplementary
group or a saved uid without knowing it.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Rely on `User=` and the image's `USER` | Nothing to write | Rejected: covers two of the ways to start the process, and says nothing about the third |
| `setuid` alone | Short | Rejected: leaves root's groups, which still grant files, and a group that is not the target's |
| `setresuid` and `setresgid` | Sets all three ids explicitly | Rejected: no better than `setuid` from root, which already sets them all, and the checks are needed either way |
| `setgroups`, `setgid`, `setuid`, then a check that the change cannot be undone | Each call only works before the next | Accepted |

## Decision

- `os::drop_privileges` calls `setgroups(0)`, `setgid` and `setuid` in that order. `setgroups`
  and `setgid` need `CAP_SETGID`, which `setuid` from root discards, so the user comes last.
- It then proves the result rather than trusting the calls: real, effective and saved uid and
  gid are all the target's; no supplementary group remains; the permitted and effective
  capability sets are empty; and `setuid(0)` and `setgid(0)` are refused. A failure of any step
  returns an error, and the caller exits, since the process is in an unknown state.
- A target with a zero uid or gid is refused. `os::resolve_user` looks a name up in the passwd
  database, and `os::drop_to_user` does both.
- It is not wired into the gateway and the worker here: their `main` functions are being
  changed by other work. The wiring is four lines each, after the options are parsed and before
  the listening socket or the first job: `if (os::is_root()) { drop_to_user(configured) }`, with
  the user from an environment variable, and a refusal to start as root when none is set.

## Consequences

- The tests run as root and skip elsewhere: changing to an arbitrary id needs `CAP_SETUID`, and
  a user namespace with one mapped id cannot express a second one.
- Threads started before the drop are covered (glibc applies each id change to all of them);
  ones started after are born unprivileged. Call it before either matters.
- Reopen if a service needs to keep one capability after the drop; the check would then name
  the capabilities it expects instead of none.

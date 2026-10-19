# 0132. The service cuts a user off

Status: Accepted
Date: 2026-10-19

## Context

A socket lives until its token's `exp` plus 60 s, and a call's participant stays in its media
room. The backend had no way to cut a user off now, on sign-out, a revoked account or abuse
(#193). Removing them from each room works, but it is per room and changes their lists.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A service API `disconnect_user`, told to every node on the members' channel | Reuses the channel every node already listens on; sockets close as an expired token closes them; calls put the user out as a removal does | A node not listening at that moment misses it | Accepted |
| Refuse tokens issued before the disconnect | Also stops reconnects with an old token | Tokens need not carry `iat`, and it needs a stored record per user | Rejected |
| LiveKit `RemoveParticipant` for calls | The rest of the call is untouched | The user's ticket still admits them until it expires | Rejected |

## Decision

- `POST /service/v1/disconnect_user` takes `{user}` and answers once
  `pg_notify('chat_members', '! <user>')` is sent.
- Each node closes the user's sockets with `4001`, from the socket's timer.
- Each node puts the user out of every call it owns that they are in, as a removal does: a
  direct call ends, and a group call moves to a new media generation without them.
- Member lists stay as they are. A node of an earlier release reads the notice as a change in
  doubt and resyncs.

## Consequences

- The backend stops issuing the user tokens, then calls `disconnect_user`; reconnecting with
  an old token works until it expires.
- A disconnect is not replayed by a resync: the backend may call it again, and it is harmless.
- `user_disconnects_total{of="socket"|"call"}` counts what each node cut off.

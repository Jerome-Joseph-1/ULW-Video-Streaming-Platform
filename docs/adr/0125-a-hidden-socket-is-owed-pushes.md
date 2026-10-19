# 0125. A hidden socket is owed pushes

Status: Accepted
Date: 2026-10-19
Amends: ADR-0111 (the registry lists the sockets in view)

## Context

A phone with the app in the background, or locked, can keep its socket open for a while. The
hook's registry counted that socket as connected, so `call_ringing` named its device as already
ringing, and the backend skipped its push. The user saw nothing, since the in-app ring is on a
screen nobody is looking at (#221). A message to such a user made no `message` event at all.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| The socket says whether it is in view, and a hidden one is left out of the registry | Uses the registry, the ring event and the message targets unchanged; the page knows (`visibilitychange`) and an app knows | A client that never says stays as today | Accepted |
| Leave the registry alone, and have every event say which devices are hidden | The backend decides | Two lists to read on every event; a message event still never goes out for a user whose only socket is hidden | Rejected |
| Guess from inactivity | Nothing for clients to do | A reader who is just reading looks idle | Rejected |

## Decision

- A socket command, `{"type":"state","visible":bool}`, answered with the same. Every socket
  starts in view, and the command is charged as a control frame.
- A hidden socket is taken out of the registry, as if closed, and put back once it says it is in
  view. Everything else about it is unchanged: it hears every frame, rings included.
- So `call_ringing` leaves its device out, `message` counts its user away if it was their only
  socket, and the service API's `presence` (ADR-0124) does not list it.

## Consequences

- A backgrounded or locked device gets the push for a ring or a message.
- A client that wants this sends `state` on `visibilitychange`, or on its app's lifecycle events.
- Each change of state is one registry write, batched as opens and closes are.

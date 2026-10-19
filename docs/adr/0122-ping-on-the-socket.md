# 0122. Ping on the socket

Status: Accepted
Date: 2026-10-19

## Context

The server pings a quiet socket after 30 s and closes it 75 s after the last thing it received.
A browser answers those pings itself and never shows them to the page. A phone in a tunnel can
hold a TCP connection that is open but dead, and the page cannot tell for over a minute that it
is offline (#227).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A `ping` command, answered `pong` with an echoed `rid` | The client picks its own timing, and needs nothing but a timer | One more frame each way per ping | Accepted |
| A shorter server ping and idle timeout | Nothing for clients to do | Every socket pays for it, and the page still learns nothing until the close | Rejected |
| Server-sent heartbeats on a fixed interval | The page sees traffic | A fixed cost on every socket, idle ones included, and the interval is the server's choice | Rejected |

## Decision

- `{"type":"ping"}`, with an optional `rid`, is answered `{"type":"pong"}` at once, `rid`
  echoed. It is answered by the session itself, so it touches no room or store.
- It is charged against the socket's control frame allowance (a burst of 20, refilling 10 a
  second). A flood is closed `1008`, as a flood of WebSocket pings is.

## Consequences

- A client that pings every few seconds, and reconnects when a pong is late, notices a dead
  socket in seconds.
- A ping counts as traffic, so a pinging socket is never closed as idle.

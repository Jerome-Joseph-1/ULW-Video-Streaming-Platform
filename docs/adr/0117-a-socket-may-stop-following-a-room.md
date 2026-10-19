# 0117. A socket may stop following a room

Status: Accepted
Date: 2026-10-19

## Context

A connection holds at most 64 joined rooms, and joins are charged to the user (a burst of 64,
then one a second). The only way to drop a room from a connection was to reconnect: `leave`
takes the user off the member list, which is a different thing. A product with groups and a room
per thread opens many rooms in one session. Reconnecting at the limit cost a rejoin, and resuming
history, for every room still shown (#214).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Raise the limit | Nothing new for clients | Moves the wall; each room a connection follows costs the node memory and fan-out | Rejected |
| `part`: drop one room from this connection, member list untouched, uncharged | What a client that stops showing a room needs | One more command | Accepted |

## Decision

- `{"type":"part","room":...}` (or `stream`, as `join` takes it) is answered
  `{"type":"parted","room":...}`. The connection stops following the room: it hears no more of
  its messages, and its place among the 64 is free at once.
- The member list is not touched, and no join is charged. A room whose member list is still
  being read for this connection's join answers `busy`; one the connection has not joined answers
  `not_joined`.
- The node keeps the room for its linger after its last subscriber here, as when a connection
  closes.

## Consequences

- A client can follow the rooms it shows on one connection, parting the rest. Joining a parted
  room again costs a join, and resuming with `after` works as for any join.

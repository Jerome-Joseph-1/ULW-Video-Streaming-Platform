# 0126. A message names who is present

Status: Accepted
Date: 2026-10-19
Amends: ADR-0111 (a `message` event may name the members with a socket open)

## Context

A `message` event went only to members with no socket in view anywhere (ADR-0111, ADR-0125).
One open laptop tab stopped every message push to that person's phone, even when they were not
looking at the laptop (#220). Chat knows which devices hold sockets. It does not know which other
devices a user has: their push subscriptions are the backend's.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| The event also names the members present, with their connected devices, and the receiver pushes to the others | Chat says what it knows; the receiver, which knows every device, decides; one read per message, as now | Present members count as told, so a receiver must read the new list once it is on | Accepted, opt-in |
| Chat decides per device | Nothing new for the receiver | Chat does not know the devices it would push to | Rejected |
| Name the present members, but do not mark them | A receiver that ignores them loses nothing | Every message in a busy room would name every member present, uncapped | Rejected |

## Decision

- `ULW_NOTIFY_MESSAGE_PRESENT=on` makes the notifier read `message_audience` in place of
  `message_targets`. It is the same statement over every member but the sender, marked told under
  the same interval, with each member's live sockets.
- The event keeps `users` (those away) and gains `present`: one entry per present member, with
  the devices their sockets named. It goes out when either list has anyone.
- Off by default, since a present member now counts as told. A receiver that reads only `users`
  would miss those members for the interval.

## Consequences

- A user reading on one device is pushed on the others.
- With the option on, a room makes at most one event per member per interval, as before, but now
  for present members too.

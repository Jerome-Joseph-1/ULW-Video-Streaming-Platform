# 0131. A direct call takes two devices of each member

Status: Accepted
Date: 2026-10-19
Amends: ADR-0050 (a direct call's media room holds four participants, two per member)

## Context

A direct call's LiveKit room held two participants (ADR-0050), one per member. Say a member
answered on their phone while their laptop was still in. Their two devices then filled the
room, and the other member could not get back in (#201).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Four participants, and at most two devices per member, counted by the owner from the SFU's listing | Phone and laptop both work; neither member can take the other's place | A listing per ticket of a call that is on | Accepted |
| Keep two, and refuse a member's second device | Simple | Moving from laptop to phone means leaving first | Rejected |
| Keep two, and put the first device out when a second comes | Seamless handover | Putting a participant out moves the media generation (ADR-0050), and a page reloaded on the same device would put itself out | Rejected |

## Decision

- A direct call's media room is opened with `max_participants` 4: `kCallParticipants` is
  `2 * kDirectDevicesPerMember`.
- A ticket for a direct call that is on lists the media room first, as a group call's does. If
  the asking member has two other devices in it, the ticket is refused `call_full`. The asking
  device itself, already in, does not count.
- A direct call that still rings is answered at once, without a listing (ADR-0113), so an early
  answer is never held up by the SFU.

## Consequences

- A member can be in a call on two devices, and the other member always fits.
- Each ticket of an answered direct call costs one `ListParticipants`.
- A third device of the same member is refused `call_full`, and the client says so.

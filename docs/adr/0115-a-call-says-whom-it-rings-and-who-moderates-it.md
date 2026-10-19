# 0115. A call says whom it rings, how many people it takes, and who moderates it

Status: Accepted
Date: 2026-10-19
Amends: ADR-0091 (a first ticket may ring some members or none; a decline may say why);
ADR-0095 (a group call's cap counts people too; the group's admins moderate it)

## Context

Several limits of calls came out of building a product on them:

- **Every first ticket rang every member** (#217, #191). A call could not ring two named people
  in a group of 20. A call could not be started quietly ("start now, share the link"): whoever
  opened the link later rang everyone again.
- **The group cap counted devices** (#200). With a cap of 3, someone on a phone and a laptop
  took two places, and the third person was refused `call_full`.
- **Only the caller moderated** (#202). A group's admin could not end a call someone else had
  started, or put anyone out.
- **A decline said nothing** (#224). The caller could not be told "can't take calls right now"
  apart from "declined", and the callee could not add a quick reply.
- **Errors did not say which call** (#229). With two requests for one room in flight, an
  error could not be matched to the call it was about.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A separate room per call for the people in it, made by the product | Works today | Two to four service calls per call, rooms that clutter `rooms`, member lists to keep in step | Rejected: the product's workaround |
| Options on the first ticket (`ring`, `max_people`), people counted from the SFU's listing, the role read with the member list, a decline's reason in the ring's notices | One ticket says it; the owner already reads the list and the listing | The call ask layout changes (layout 4) | Accepted |

## Decision

- **How a call rings.** `call` gains `ring`. `true` rings every other member, the default.
  `false` rings nobody, and the call is on at once. A list of user ids rings only those, up to
  32; ids not on the room's member list are left out, and a list that names nobody left rings
  nobody. A call that rings nobody counts against no ring limit. Whoever comes first, a direct
  call's other member or anyone in a group, is `call_answered` for everyone. Like any call
  (ADR-0113), it ends when nobody is in it.
- **People.** A group call's cap on people, each user counted once whatever their devices, is
  `ULW_CALL_GROUP_PEOPLE` (2 to 16, by default the device cap). A call's first ticket may set it
  lower with `max_people`. The owner counts the users the SFU lists, other than the asker. A
  ticket for someone new past the cap is `call_full`; another device of someone already in the
  call is not counted again. The device cap stands as before.
- **Moderation.** The owner reads the asker's role with the member list it reads anyway
  (`RoomAccess::role`). A group's admins may end its call for everyone and put members out, as
  its caller may.
- **A decline's reason.** `call_decline` and `decline_call` take an optional `reason`
  (`declined`, `busy` or `not_allowed`) and `text`, a quick reply of at most 200 bytes.
  `call_declined` carries them to every socket and to the hook. A `Declined` notice that carries
  them gains trailing bytes, as a `Cancelled` notice with a reason does (ADR-0113).
- **Echoes.** Every call command takes `rid`, the client's id for the request (a message id's
  syntax). It is echoed in the answer, error or not, and an error names the `call` it was about.
- The call ask layout moves to 4: a ticket ask carries its ring options, and a decline its
  reason and text.

## Consequences

- A node of the previous release refuses every call ask of layout 4 as malformed, and the asker
  is answered `unavailable`. This release is a `Recreate` rollout already (ADR-0112), so there is
  no such window.
- A disconnected member's device still counts in the device cap until LiveKit drops it, as
  before. The people cap counts that member once.
- Who may moderate is read when the moderation is asked, never cached: an admin demoted since
  the call started may no longer moderate it.

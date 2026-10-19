# 0113. A call lasts while someone is in its media room

Status: Accepted
Date: 2026-10-19
Amends: ADR-0091 (a direct call's ring ends when its caller is not in the call, and an answered
call is no longer forgotten at its hold); ADR-0095 (a group call nobody answered stays on while
its caller is in it)

## Context

The owner kept a call by its tickets alone (ADR-0091). Only a group call's owner asked the SFU
who was connected, and only once it was answered (ADR-0095). Four failures followed, all of
which the room's own LiveKit room could have told apart:

- **A ring with nobody behind it** (#187). A caller whose page closed, reloaded or lost its
  socket before `call_cancel` got out left the callee ringing for the full 45 s, and an answer
  joined an empty room. livekit-client leaves the room on `beforeunload` by itself, so the
  caller was gone from the media room long before the ring timed out.
- **An answered direct call forgotten mid-call** (#191). Two minutes after its last ticket the
  owner forgot it, while both members were still talking. `call_end` was then `no_call`, and
  the next ticket rang as a new call.
- **A group ring that ran out ended the call** (#191, ULW-44). The caller, still waiting in
  LiveKit, was in a call chat said was over. Whoever opened the call later rang everyone again.
- **A call nobody was in, rejoined silently** (#225). After everyone closed their tabs without a
  word, the next "call" within the hold named the old call: `call_answered` by the asker, nobody
  rung, and the asker alone in the room.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| LiveKit's `participant_left` webhooks into chat | Immediate | Chat has no webhook endpoint; LiveKit posts to one URL, which the live-stream component owns; a lost webhook is a ring that never ends | Rejected for now |
| The owner asks the SFU who is in the call's room, as group calls already do, at the moments that matter | One mechanism for every call; the adapter's `participants` exists; a check that fails changes nothing | Up to about 20 s to notice a caller who left; one SFU read per ringing call every 5 s | Accepted |
| Shorter tickets and holds | No new reads | Every hold is a guess; the faults stay, only shorter | Rejected |

## Decision

- **A direct call's ring.** From `caller_grace` (10 s) into the ring, and every `caller_check`
  (5 s) after, the owner asks the SFU who is in the call's room. While a direct call rings only
  its caller holds a ticket. Nobody listed twice in a row (`kCallerAbsentChecks`) ends the ring:
  `call_cancelled` with no `by` and `reason: "caller_left"`, on the sockets and the hook. A check
  the SFU cannot answer counts for neither side, and the ring times out as before.
- **An answer.** A callee's ticket asked `caller_grace` or more into the ring is issued only if
  the SFU lists the caller in the room. Otherwise it is refused `no_call`, and the ring ends as
  above. One asked earlier is issued at once: the caller may still be connecting.
- **An answered call** lasts while the SFU lists anyone in its room. The hold (`answered_hold`,
  2 minutes after the last ticket, then `occupancy_check`, 30 s) is when the owner asks, no
  longer when it forgets. Nobody listed, or three checks unanswered, ends it: `call_ended` with
  no `by`. The owner keeps the room's media handle for as long as the call lasts.
- **A group ring that runs out** with nobody but the caller in the call ends the ring
  (`call_missed` to everyone, as before) but not the call. The call is on from then, and the SFU
  is asked at once. Whoever joins later is `call_answered`; nobody there is `call_ended`.
- **A ticket asked again of a call that is on**, without `answer`, lists the room first. Nobody
  listed ends that call (`call_ended`, no `by`), and the ticket starts the room's next call,
  which rings.
- **Fresh tickets.** Within `caller_grace` of the call's latest ticket, nobody listed proves
  nothing: that device may be connecting. Then a check counts for neither side, and a ticket
  asked again joins the call as it stands.
- A `call_cancelled` notice that carries a reason gains one trailing byte. Every other notice is
  laid out as before.

## Consequences

- A caller whose page goes away stops ringing the callee within about 20 s, and an answer never
  lands in an empty direct call. calls.md says how to leave cleanly (`disconnectOnPageLeave:
  false`, then `call_cancel` or `call_end` from `pagehide`); this is what happens when that does
  not get out.
- `call_end` of an answered call works for as long as the call does. A call is no longer
  forgotten while anyone is in it.
- Each ringing direct call costs one `ListParticipants` every 5 s from 10 s in, at most 7 per
  ring. Each answered call costs one every 30 s once its last ticket is 2 minutes old.
- A node of the previous release drops the `call_cancelled` notices that carry a reason. Its
  members' devices stop ringing at `expires_at`, as they did. A rollout with `Recreate`, as the
  node channel's version 6 (ADR-0112) needs, has no such window.
- New counters: `call_rings_total{outcome="caller_left"}` and `{outcome="stale"}`.

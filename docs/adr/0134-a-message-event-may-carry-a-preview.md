# 0134. A message event may carry a preview

Status: Accepted
Date: 2026-10-22

## Context

The hook's `message` event never carries the body (ADR-0111), so a push can only say "New
message from …" unless the backend reads the message itself (#226). Many products want the
first words in the notification. Bodies are opaque to chat, and in a room whose devices use MLS
(ADR-0016) they are ciphertext. Chat records no per-room flag that says so: E2EE is the
clients' convention, and every body there is one MLSMessage.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Opt-in `ULW_NOTIFY_MESSAGE_PREVIEW_CHARS`: the sender's node keeps the first N characters of the body from its send and puts them in `message` | No store read; the event names one message, and that one is what is previewed | Message text leaves chat for the receiver; only text bodies have one | Accepted, off by default, at most 200 |
| Read the newest stored body when the event goes out | Previews what is newest in the room | One more statement per event; a quiet edit or reaction may be newer than the message | Rejected |
| Never for direct and group chats, which ADR-0016 encrypts | Simple | Those are the only rooms `message` is for, so the option would do nothing | Rejected |
| A body is previewed only when its start is text: valid UTF-8 with no control character but tab and line breaks | An MLSMessage starts `00 01` and is never text, so E2EE rooms get no preview without a per-room flag | A plaintext body with a control character gets none | Accepted |
| A per-room opt-out, `set_room_notify {room, preview:false}` | A product can keep some rooms out | Needs a stored per-room setting (a migration) and a read on every event | Left out |

## Decision

- `ULW_NOTIFY_MESSAGE_PREVIEW_CHARS`, 0 (the default, off) to 200, only with `ULW_NOTIFY_URL`.
- While it is above 0, `ChatService::send` keeps `message_preview(body, N)` with each loud send
  outside a stream's live chat, and gives it to the notifier with the seq. A quiet message or a
  live chat's keeps none.
- `message_preview` takes the first N code points, never cutting one. It gives nothing for an
  MLS header (`00 01 00 01`..`05`), invalid or overlong UTF-8, a surrogate, a value past
  U+10FFFF or a control character other than tab, LF and CR among those N. Bytes after them are
  not read.
- `message` gains `preview` when there is one, with or without `ULW_NOTIFY_MESSAGE_PRESENT`.

## Consequences

- With it on, the start of a message's text reaches the receiver. Run it behind the
  NetworkPolicy notifications.md already asks for. An E2EE room's bodies never do.
- The preview is of the message the event names, the first in the interval that owed someone
  word. Later messages in the interval make no event, so they have no preview.
- A preview may cut a grapheme (an emoji with a modifier, a combining accent) between its code
  points. The receiver adds its own ellipsis.
- Left out: a per-room opt-out (`set_room_notify`). A product that wants one keeps its own list
  and drops `preview` for those rooms.

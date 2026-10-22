# 0133. The backend reads and hears every message

Status: Accepted
Date: 2026-10-22

## Context

A backend that acts on a message (a tip, a report, a bot's reply) must first confirm that
"seq N exists in room R and was sent by X" (#213). It could not: the service API reads member
lists but no history, and the hook's `message` event is for pushes. It goes only to members
with no socket, at most once per member per interval, and names no message id. Nothing in chat
told the backend about every message, and nothing let it look one up.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A service op `history`, paged as the socket's `history` is, with the bodies only when asked | One store read the socket already makes; the backend checks any claim, and fills any gap the hook left | A poll: it says nothing on its own | Accepted |
| An opt-in hook event `message_sequenced` from the room's owner, for every message, no body | The owner sequences every seq exactly once, whichever node its sender is on; no lookup, no throttle, no extra statement | At most once, like every hook event; a busy deployment sends one event per message | Accepted, off by default |
| The same event from the sender's node, beside `message` | The `message` path already runs there | A sender whose socket closes before the answer is never told of, so neither is the backend; a repeated send would be told twice | Rejected |
| Give `message` a message id and drop its throttle | One event | Changes what every receiver of `message` gets, and the push path wants the throttle | Rejected |
| Stream live chats in `message_sequenced` too | "Every message" | Thousands of senders in one room would crowd calls and messages out of the hook's 4096-event queue | Rejected: `history` reads them |

## Decision

- `POST /service/v1/history` takes `room` and optional `before` or `after` (one seq, not both),
  `limit` (1 to 100, default 50) and `bodies` (`false` by default). It reads
  `IMessageStore::history_before` or `history_after` and answers
  `{"type":"history","room":..,"messages":[{"seq","sender","id","at","quiet"[,"body"]}]}`: newest
  first without a cursor or with `before`, oldest first with `after`, and an empty page at the
  end. `at` is Unix milliseconds and `body` base64url, as on the socket. Any room but a
  presence room.
- `rt::IRouterEvents::on_sequenced` tells the owner of each seq it takes and stores, once,
  before any member hears it, and never for a repeat. `RoomLog` passes it to the notifier.
- With `ULW_NOTIFY_SEQUENCED=on` (off by default; only with `ULW_NOTIFY_URL`), the notifier
  makes `message_sequenced`, `{room, seq, sender, message_id, sent_at, quiet}`, for each one in
  a direct or group chat. It has no body. `message_id` is the sender's `id` and `sent_at` is
  Unix milliseconds: the event's own `id` and `at` keep their meaning.

## Consequences

- Turning the event on costs one event per message on each room's owner. Nothing more is read
  or written.
- Events are still at most once. A seq whose owner died between storing it and telling it, or
  a lost batch (`lost`), is missed. Reconcile with `history` and `after`.
- `history` costs one read of the service's allowance, the same as `members`. A page with bodies
  is up to about 350 KiB on the wire.

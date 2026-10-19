# 0118. A history page around a seq

Status: Accepted
Date: 2026-10-19

## Context

`history` took `before` or `after`, not both. A view centred on one message, such as "jump to
the message this reply quotes", needed two requests. Each was charged against the join allowance
and took its own round trip (#219).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Allow `before` and `after` together | No new name | Two limits to split, and two orders in one answer | Rejected |
| `around` and `limit`: the page centred on the seq, oldest first | What the view asks for, one request | Two store reads behind one answer | Accepted |
| One SQL statement around a seq | One read | A third query in each store, for a page that is rare | Rejected for now |

## Decision

- `history` gains `around`, a seq, in place of `before` and `after`. The node reads the seq and
  those below it (half the page, rounded up), then those above it (the rest), and sends them
  oldest first, then `history` with the count.
- It is charged as one request. A store that fails either read fails the page, `unavailable`.

## Consequences

- Two reads per such page; their bounds are the page's own (`limit` at most 100).

# 0120. Video events on the notification hook

Status: Accepted
Date: 2026-10-19
Amends: ADR-0111 (the hook carries videos' events too)

## Context

A backend learned that a video was ready or had failed only by polling the service listing
(ADR-0100). So it needed a reconciler running in the background, and a lesson could only be
published some time after it was ready (#208). Chat already POSTs signed events to the
operator's receiver (ADR-0111). Video states are written by the worker and by the upload
reaper, neither of which has the hook.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| The worker POSTs its own hook | The event leaves where the state is written | A second sender, its own secret and egress from the sandboxed worker; the reaper's failures need a third | Rejected |
| A trigger writes each change to a table, and chat nodes with the hook on take it and send it | One receiver, one signature, one NetworkPolicy; every writer covered, the reaper's included; nothing in the worker | A video event is up to a poll (2 s) late; chat sends videos' events | Accepted |
| LISTEN/NOTIFY to every chat node | No poll | Every node hears every change, so each would be sent once per node | Rejected |

## Decision

- **Migration 0023.** A trigger on `videos` writes a row to `video_events` when a video that is
  not deleted becomes `ready` or `failed`. Each write first deletes the rows older than an hour,
  so a deployment without the hook keeps at most an hour of them.
- **Taking.** Every chat node with `ULW_NOTIFY_URL` set takes up to 64 rows every 2 s, at once
  again after a full batch. One statement deletes them, skipping the rows another node is taking,
  so each change goes to one node. The rows come out oldest first.
- **Events.** `video_ready` (with `duration_ms`) and `video_failed` (with `error_reason`). Each
  names the `video`, its `owner` and its `version`. `at` is when the video changed. They go out
  in the same batches, signed the same way, and at most once, as every hook event (ADR-0111).

## Consequences

- A backend can publish a video when `video_ready` arrives. The listing is still the source of
  truth: an event lost with a down receiver is not sent again.
- Each chat node with the hook on runs one small statement every 2 s.
- Counters `notify_video_events_total` and `notify_video_takes_failed_total`.

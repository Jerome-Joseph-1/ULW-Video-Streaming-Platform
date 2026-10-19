# 0119. One visibility for many videos

Status: Accepted
Date: 2026-10-19

## Context

The service API sets one video's visibility per request (ADR-0100). Publishing or unpublishing
a course means one `PATCH` per lesson, so a backend writes a loop, its retries, and a half-done
state to report when the loop stops partway (#209).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A collection `PATCH` that applies the single PATCH to each listed video and reports each | One request; each video checked exactly as now; nothing new in the catalog | Not atomic: some videos may change and others not | Accepted |
| One transaction for every video, all or none | Never half done | A room check per video inside one transaction; one missing video refuses the whole course | Rejected |
| A set of videos the backend names once (a "course") | Fewer ids per request | A new object in the catalog, whose lifetime is the product's business | Rejected |

## Decision

- `PATCH /api/v1/service/videos` takes `{"videos": [<id>, ...], "visibility": ...}`: 1 to 50
  distinct ids and the single PATCH's visibility, in the same 4 KiB body. A bad visibility is
  `400` `bad_visibility`; a list missing, empty, longer than 50, with a malformed or a repeated
  id is `400` `bad_videos`. Only the service's bearer token may use it.
- Each video is changed in turn, as the single PATCH changes it, with the same room check.
- Once the body is good the answer is `200`, with `videos` (each one changed, as its owner sees
  it, in the order asked) and `failed` (each one not, with `not_found`, `not_member` or
  `unavailable`). An `unavailable` one is worth a retry, of that id alone.

## Consequences

- A backend publishes a course in one request, and retries only what `failed` names.
- A request is up to 50 catalog writes in a row, so it takes up to 50 round trips to Postgres.
- The answer is never a refusal for one video. A backend must read `failed`, not the status.

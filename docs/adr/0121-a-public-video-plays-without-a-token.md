# 0121. A public video plays without a token

Status: Accepted
Date: 2026-10-19
Amends: ADR-0097 (a fourth visibility, `public`)

## Context

A video was `private`, `unlisted` (any signed-in user) or shared with a room (ADR-0097), so
every playlist needed a token. A course trailer, a landing page's clip or an embed on a public
page could not play for a visitor who was not signed in (#207).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A `public` visibility, and no token needed for that video's metadata and playlists | One rule in `access_of`; set as any visibility, by the owner or the backend; segment URLs already need no token | Requests without a token reach the catalog, bounded by the address's limits | Accepted |
| Long-lived presigned playlist URLs the backend hands out | No change to who may call | A URL to rotate per video and per rendition, valid after a video is made private | Rejected |
| An unauthenticated CDN path to the stored playlists | No gateway work | The stored playlists hold no signed segment URLs, and a takedown would not reach the CDN | Rejected |

## Decision

- `Visibility` gains `public`. `access_of` treats it as `unlisted` for a signed-in viewer, and
  `access_of_anonymous` serves a public video once its upload is committed, as a viewer, and
  nothing else (no grant or room names an anonymous caller).
- `GET /api/v1/videos/{id}` and both playlists accept a request with no token at all (no
  `Authorization` header and no auth cookie). Without a token, anything but a public video is
  `401` with `WWW-Authenticate: Bearer`, as before, and so is a missing video, so the answer
  says nothing of whether an id exists. A token that fails is `401`, never treated as none.
- Requests without a token stay under their address's limits and are not recorded as views.
- A live stream's visibility is never public: `400` `bad_visibility`, and the schema's check is
  unchanged.
- Migration 0024 widens `videos_visibility` to allow `public`.

## Consequences

- An operator's public page embeds a player pointed at the master playlist.
- A gateway of the previous release reads a public video's row as corrupt (`500`). Roll the
  gateways out before anyone sets `public`, and roll back only after no video is public.
- Anonymous plays are not counted as views.

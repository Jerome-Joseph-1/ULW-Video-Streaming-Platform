# Versioning and compatibility

## What the code does today

<!-- apps/gateway/src/routes.hpp, apps/gateway/src/connection.cpp (start_create), apps/chat/src/envelope.cpp -->

- **HTTP API.** Every public path starts with `/api/v1`. There is no version header and no
  content negotiation; the version is the path.
- **Request JSON (gateway).** `POST /api/v1/uploads` reads the fields it knows and ignores the
  rest, so a client may send extra fields without breaking.
- **Response JSON (gateway).** Objects are written by the server with a fixed field set. Field
  order is not significant.
- **Chat envelope.** No version field. The server refuses unknown `type`s and unknown fields
  with `error` `malformed`, so a client cannot send fields the server does not know. The envelope
  ([chat.md](chat.md#messages)) is Stable (ADR-0036, ADR-0043). Like the HTTP API it may gain
  fields, message `type`s and `error` reasons under rule 1 below; a client ignores the ones it
  does not know, and a request field the server adds is optional to send.
- **Playlists.** HLS as the worker writes it: fMP4 segments, an `EXT-X-MAP` init segment, a
  master with one variant per rendition. Rendition names are `<height>p`.

## Releases

<!-- .github/workflows/release.yml, .github/workflows/publish-images.yml, docs/adr/0099-releases-are-version-tags-on-published-images.md -->

A release is a version, `vMAJOR.MINOR.PATCH`, given to a commit on `main` and to the four images
already published for it (ADR-0085, ADR-0099). Each has a
[GitHub Release](https://github.com/Jerome-Joseph-1/ULW-Video-Streaming-Platform/releases) listing:

- the commit;
- each image, `ghcr.io/jerome-joseph-1/ulw-<service>:<version>`, and the same image as
  `<sha>@sha256:<digest>`, which is what a production environment pins
  ([RUNBOOK](../../deploy/kubernetes/RUNBOOK.md), 4a);
- how to check each digest's build provenance: `gh attestation verify oci://<image>@<digest>
  --repo Jerome-Joseph-1/ULW-Video-Streaming-Platform`;
- this directory and [changelog.md](changelog.md) as they were at that release, and the pull
  requests merged since the previous one.

A version never changes: its tag in the repository and its image tags name one commit and one
digest each, and a fix ships as the next version. The number follows the policy below once it is
agreed: a breaking change to a stable surface is a new major version, an addition a new minor, a
fix a new patch.

**Cutting one** (the repository's writers): publish-images must have published the commit
(every push to `main` is, once its `ci` run succeeds, which starts the publish). Then Actions, release, "Run workflow" from
`main`, with `version` and, for anything but main's tip, `commit`. The run refuses a commit that
is not on `main`, whose `ci` run did not succeed or whose images are missing, and a version that
exists already; it builds nothing.

## Stable and draft

| Surface | Status |
|---|---|
| Upload endpoints ([uploads.md](uploads.md)) | Stable under `/api/v1` |
| Video and playback endpoints ([videos-and-playback.md](videos-and-playback.md)) | Stable under `/api/v1` |
| Auth ([auth.md](auth.md)) | Stable |
| Probes and metrics ([operator-contract.md](operator-contract.md)) | Stable paths; metric set may grow |
| Chat: the WebSocket envelope and the service API ([chat.md](chat.md)) | Stable |
| Calls, 1:1 and group ([calls.md](calls.md)) | Stable |
| Live streams: the stream endpoints under `/api/v1/live`, publishing over WHIP, live playlists and recordings ([live.md](live.md)) | Stable |
| E2EE ([e2ee.md](e2ee.md)) | Draft until the milestone named at the top of the page merges |

## Proposal: compatibility policy

*This is a proposal. Nothing in the code enforces it yet; it becomes policy once the
project's maintainers and the teams integrating it agree to it.*

1. **Additive changes need no new version.** Within `/api/v1` the service may add endpoints,
   add fields to response objects, add optional request fields, add values to `state` or to
   chat `error` reasons, add response headers, and add metrics. Clients must ignore response
   fields they do not know and treat an unknown `state` as "not ready yet".
2. **Breaking changes get a new path prefix.** Removing or renaming a field, endpoint or header,
   changing a field's type or meaning, tightening validation of input that was accepted, or
   changing a status code a client acts on, ships as `/api/v2/...`. `/api/v1` keeps working
   beside it for at least 90 days after `v2` ships in a release.
3. **Announcement.** A breaking change is announced to the teams integrating it at least 30
   days before `v2` ships in a release, with a changelog entry in this directory listing every
   difference and the date `v1` goes away. Security fixes may shorten this; they are announced
   as soon as they ship, in [changelog.md](changelog.md).
4. **Chat envelope.** From M17 on, the envelope is versioned by the WebSocket path: `/rt` is
   version 1 of the final envelope, and a breaking change moves to `/rt/v2` with the same notice
   period. Adding a message `type` or an optional field is additive, but because the server
   refuses unknown fields, a client may only send a new field once the server that knows it is
   deployed everywhere; the changelog says when.
5. **Deprecation signal.** A deprecated endpoint answers with a `Deprecation` header (RFC 9745)
   and a `Sunset` header (RFC 8594) carrying the removal date, from the day it is announced.

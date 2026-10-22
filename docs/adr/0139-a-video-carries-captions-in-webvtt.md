# 0139. A video carries captions in WebVTT

Status: Accepted
Date: 2026-10-22

## Context

The pipeline made no text track: a video had no captions for viewers who need them, nor a
transcript to search (#210). HLS lists subtitles as a rendition of the master, an
`EXT-X-MEDIA` of `TYPE=SUBTITLES` whose media playlist names WebVTT files, and every player the
guide covers (hls.js, Safari, AVPlayer, ExoPlayer) shows such a track.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| An uploaded WebVTT file per video and language, through the owner's and the service's API, checked, stored next to the renditions, its languages in the catalog and listed in the master the gateway serves | Captions every product already has (from an editor, a vendor, a human transcriber) play now; one rule for who may see them, the video's; nothing changes in the worker | The product has to bring the file; a file the check refuses has to be fixed by its author | Accepted |
| Speech to text in the worker | Captions without anyone writing them | A model and its runtime in the worker image, minutes of CPU per hour of video on a box sized for ffmpeg, quality to vouch for in every language; out of scope here | Rejected |
| Captions inside the renditions (CEA-608/708 in the H.264 stream) | No separate file | Burned in at transcode time, so a fix means transcoding again; one language per channel, ASCII-ish | Rejected |
| The languages as objects listed from the store at each master request | No table | A LIST per master fetch, a class A operation, ten times a GET's price | Rejected |
| The gateway rewrites the stored master when a file is put | No read at playback | A read-modify-write the worker and two uploads race on; a new rendition rewrites every master | Rejected |

## Decision

- `PUT /api/v1/videos/{id}/captions/{language}` takes the file as its body, the owner only;
  `DELETE` removes it. `PUT` and `DELETE /api/v1/service/videos/{id}/captions/{language}` do
  the same for the operator's backend, for any video. Both answer `204`. A viewer gets `403`,
  anyone else the `404` a missing video gets, as for the visibility.
- `{language}` is a BCP 47 tag of the plain shape: 2 or 3 letters, then subtags of 1 to 8
  letters or digits after `-`, at most 35 characters, kept lowercase (`core::CaptionLanguage`).
  Anything else is `400` `bad_language`.
- The body is at most 512 KiB (`413` past it, `400` empty) and is checked before anything is
  looked up: UTF-8 without NUL, the `WEBVTT` signature, every block a cue, a `NOTE`, or a
  `STYLE` or `REGION` ahead of the first cue, every timing `[hh:]mm:ss.ttt --> [hh:]mm:ss.ttt`
  with its end after its start, cues in the order they start, and no `-->` anywhere else. A
  refusal is `400` `{"error":"bad_captions","reason":"<encoding|header|timing|order|block>",
  "line":<n>}`. The text and settings of cues are not looked into.
- While its body is read and held, a `PUT` holds one of the user's upload slots, as a chunk
  does: at most three per user and `max_upload_slots` in all, `429` or `503` past them.
- The file goes to `videos/<id>/hls/captions/<language>.vtt` before the catalog lists the
  language, and the catalog unlists it before the file is removed, so a language listed always
  has its file. Migration 0030 adds `video_captions` (video, language); its rows go with the
  video's at the reaper's purge, its files with the rest of `videos/<id>/`.
- The master playlist lists every language as
  `EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="subs",NAME=<tag>,LANGUAGE=<tag>,DEFAULT=NO,AUTOSELECT=YES`,
  ahead of the first variant, and every `EXT-X-STREAM-INF` names the group. Its `URI` is
  `/api/v1/videos/{id}/captions/{language}/index.m3u8`, which the gateway builds itself: one
  segment, the file's presigned URL, as long as the video, signed for as long as its segments
  are. Captions may be put in any state; their playlists, like the video's, are `409` until it
  is ready.
- Every playlist request reads the languages from the catalog, after the same access check as
  the video's: whoever may play the video plays its captions, a public video's without a token.

## Consequences

- Captions are what the product uploads: nothing in the platform writes them, and speech to
  text stays out of scope. A product that wants transcripts generated runs a service of its own
  and puts the result.
- A cue's times are the video's own, from 0; there is no `X-TIMESTAMP-MAP` offset to set. The
  file is also the transcript: a client that wants the text fetches it from the playlist's URL.
- Each language is named by its tag (`NAME="en"`): a player that does not localize tags shows
  the tag. Labels, `FORCED` and `CHARACTERISTICS` for captions for the deaf are a later change.
- A master request makes one more catalog read, by primary key.
- The store serves the file with its default content type, not `text/vtt`; the players above
  read it whatever the type. The bucket's CORS rule for segments covers it.
- A `PUT` racing the video's deletion may leave its file after the reaper's purge listed the
  video's objects; nothing names it, and its URL was never handed out.
- `caption_changes_total{op}`, `captions_rejected_total` and
  `playlist_requests_total{kind="captions"}` count what happens.
- A gateway of the previous release answers the new routes `404` and serves masters without the
  subtitles group; roll the gateways out, then tell the product, and migration 0030 must run
  first.

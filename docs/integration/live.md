# Live streams

> **Draft: changes until milestone M33 merges.** Nothing on `main` serves live streams yet. This
> page states the model only.

A broadcaster publishes into the realtime tier (WHIP first, RTMP as a fallback). A packager
turns the stream into HLS and writes its segments to the same object store VOD uses. Viewers
never receive WebRTC: they play live HLS from the store (ADR-0014), fetched the same way as VOD
([videos-and-playback.md](videos-and-playback.md)): playlists through the gateway, segments from
presigned store URLs. Live playlists are cached for less than VOD's 60 s. When a stream ends its
recording becomes an ordinary video that goes through `processing` to `ready`. Live chat is a
chat room in lossy delivery mode ([chat.md](chat.md)).

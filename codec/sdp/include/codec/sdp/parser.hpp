#pragma once

#include "codec/sdp/error.hpp"
#include "codec/sdp/session.hpp"

#include <cstddef>
#include <expected>
#include <string_view>

namespace codec::sdp {

struct Limits {
    // Chromium 141's offer for audio, three-layer simulcast video and a data channel is 6,006
    // bytes, 4.6 KB of it the video section. Eight video sections are 37 KB; 48 KiB covers
    // them and, JSON-escaped (two more bytes per line, under a thousand lines), still fits in
    // the 64 KiB WebSocket message that carries it.
    std::size_t max_bytes = std::size_t{48} * 1024;
    // A call sends audio, camera and screen each way (six sections) plus a data channel; one
    // more leaves room for a section a renegotiation stopped.
    std::size_t max_media_sections = 8;
    // Per section, and at session level. Chromium's video section carries 114: 23 formats,
    // most with an rtpmap, an fmtp and five rtcp-fb lines. 256 is a little over twice that.
    std::size_t max_attributes = 256;
};

// Parses and validates an offer or an answer: the grammar of RFC 8866 and of each typed
// attribute, then what the signalling boundary enforces (see validate.cpp). Anything accepted
// serializes back to the same text, with CRLF line endings.
[[nodiscard]] std::expected<Session, Error> parse(std::string_view text, const Limits& limits = {});

} // namespace codec::sdp

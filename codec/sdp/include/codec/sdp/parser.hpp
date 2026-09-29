#pragma once

#include "codec/sdp/error.hpp"
#include "codec/sdp/session.hpp"

#include <cstddef>
#include <expected>
#include <string_view>

namespace codec::sdp {

struct Limits {
    // The largest room planned is six people with screen share: each peer's audio, camera and
    // screen, 5 remote * 3 + 3 of ours + a data channel = 19 sections, 12 of them video. In
    // Chromium 141's offer a video section is 4.1 KB, audio 1.3 KB and the data channel
    // 0.35 KB, so 12 * 4.1 + 6 * 1.3 + 0.35 is 57 KB (56.0 KB built from the fixture; a Pion offer
    // with five remote participants, 11 sections, is 27 KB, which agrees). The text travels
    // JSON-escaped in one WebSocket message of at most 64 KiB (ADR-0029): each CRLF grows by two
    // bytes and that room has about 1,700 lines, +3.4 KB, plus a couple of hundred bytes of
    // envelope. 58 KiB is 59.4 KB, over the room's 57 KB; escaped it is 62.8 KB, under 65.5 KB.
    std::size_t max_bytes = std::size_t{58} * 1024;
    // The 19 sections above, and room for those a renegotiation stopped. It is the byte limit
    // that bounds a full-size description: 32 video sections would be 131 KB.
    std::size_t max_media_sections = 32;
    // Per section, and at session level. Chromium's video section carries 114: 23 formats,
    // most with an rtpmap, an fmtp and five rtcp-fb lines. 256 is a little over twice that.
    std::size_t max_attributes = 256;
};

// Parses and validates an offer or an answer: the grammar of RFC 8866 and of each typed
// attribute, then what the signalling boundary enforces (see validate.cpp). Anything accepted
// serializes back to the same text, with CRLF line endings.
[[nodiscard]] std::expected<Session, Error> parse(std::string_view text, const Limits& limits = {});

} // namespace codec::sdp

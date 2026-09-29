// Whatever the input, parse() either refuses it at a line that exists, or accepts it and then
// keeps the codec's promise: serializing gives back the input, byte for byte, with every line
// ended by CRLF; and parsing that gives the same Session again.
//
// Input: the description, as it would arrive at the signalling boundary.

#include "codec/sdp/parser.hpp"
#include "codec/sdp/serializer.hpp"
#include "codec/sdp/session.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

void check(bool invariant) {
    if (!invariant) {
        __builtin_trap();
    }
}

// The input as the serializer would write it: the parser's lines, each ended by CRLF.
std::string with_crlf(std::string_view text) {
    std::string out;
    while (!text.empty()) {
        const std::size_t lf = text.find('\n');
        std::string_view line = text.substr(0, lf);
        text = lf == std::string_view::npos ? std::string_view{} : text.substr(lf + 1);
        if (lf != std::string_view::npos && line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        out += line;
        out += "\r\n";
    }
    return out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // libFuzzer hands over bytes; SDP is text in whatever encoding those bytes are.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    const auto session = codec::sdp::parse(text);
    if (!session) {
        // A missing line is reported one past the last.
        const bool unterminated = !text.empty() && !text.ends_with('\n');
        const auto lines =
            static_cast<std::size_t>(std::ranges::count(text, '\n')) + (unterminated ? 1U : 0U);
        check(session.error().line <= lines + 1);
        return 0;
    }
    const std::string written = codec::sdp::serialize(*session);
    check(written == with_crlf(text));
    const auto again = codec::sdp::parse(written);
    check(again.has_value() && *again == *session);
    return 0;
}

// Feeds arbitrary text to the playlist rewriter as a stored master and as a stored media
// playlist, and checks what it lets through: every signed key lies under the playlist's
// directory, no rewritten playlist holds a blank line or a control character, and every URI line of
// the output is one the rewriter produced itself.
//
// Input: byte 0 selects which signer refusal to simulate (0 means none); the rest is the text.

#include "core/util/hls.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kDir = "videos/v/hls/r/";
constexpr std::string_view kRoute = "/api/v1/videos/v/";
constexpr std::string_view kSigned = "https://s.example/";

void check(bool invariant) {
    if (!invariant) {
        __builtin_trap();
    }
}

// Each line that does not start with '#' must start with `prefix`, and none may be empty.
void check_lines(std::string_view out, std::string_view prefix) {
    check(out.starts_with("#EXTM3U\n"));
    check(out.ends_with('\n'));
    while (!out.empty()) {
        const std::size_t nl = out.find('\n');
        const std::string_view line = out.substr(0, nl);
        out.remove_prefix(nl + 1);
        check(!line.empty());
        for (const char c : line) {
            check(static_cast<unsigned char>(c) >= 0x20);
        }
        if (!line.starts_with('#')) {
            check(line.starts_with(prefix));
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    // The bytes are text to the rewriter, of whatever encoding.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view input{reinterpret_cast<const char*>(data), size};
    const std::size_t refuse_at = static_cast<unsigned char>(input[0]);
    const std::string_view text = input.substr(1);

    std::size_t calls = 0;
    const auto media = core::hls::rewrite_media(
        text, kDir, [&](const core::StorageKey& key) -> std::optional<std::string> {
            check(key.view().starts_with(kDir));
            check(key.view().size() > kDir.size());
            if (++calls == refuse_at) {
                return std::nullopt;
            }
            return std::string(kSigned) + key.str();
        });
    if (media) {
        check_lines(*media, kSigned);
        check(refuse_at == 0 || calls < refuse_at);
    } else if (media.error() == core::hls::PlaylistError::Unsigned) {
        check(calls == refuse_at);
    }

    const auto master = core::hls::rewrite_master(text, kRoute);
    const auto names = core::hls::list_renditions(text);
    if (master) {
        check_lines(*master, kRoute);
        check(names.has_value());
    }
    if (names) {
        for (const std::string_view name : *names) {
            check(!name.empty() && name.find('/') == std::string_view::npos);
            check(name != "." && name != "..");
        }
    }
    return 0;
}

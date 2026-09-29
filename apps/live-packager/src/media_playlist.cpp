#include "media_playlist.hpp"

#include "core/util/parse.hpp"

#include <format>
#include <utility>

namespace live {

namespace {

constexpr std::string_view kTargetDuration = "#EXT-X-TARGETDURATION:";
constexpr std::string_view kMediaSequence = "#EXT-X-MEDIA-SEQUENCE:";
constexpr std::string_view kDiscontinuitySequence = "#EXT-X-DISCONTINUITY-SEQUENCE:";
constexpr std::string_view kDiscontinuity = "#EXT-X-DISCONTINUITY";
constexpr std::string_view kMap = "#EXT-X-MAP:";
constexpr std::string_view kProgramDateTime = "#EXT-X-PROGRAM-DATE-TIME:";
constexpr std::string_view kExtInf = "#EXTINF:";
constexpr std::string_view kEndList = "#EXT-X-ENDLIST";

constexpr std::int64_t kMicrosPerSecond = 1'000'000;
// Six fractional digits, which is what ffmpeg writes and what fits a microsecond count.
constexpr std::size_t kFractionDigits = 6;
// Far past any segment, and small enough that its microseconds cannot overflow.
constexpr std::uint64_t kMaxDurationSeconds = 3600;

// "2.000000" -> 2,000,000 us. Digits past the sixth are dropped.
std::optional<Micros> parse_duration(std::string_view text) {
    const std::size_t dot = text.find('.');
    const auto whole = core::parse_integer<std::uint64_t>(text.substr(0, dot));
    if (!whole || *whole > kMaxDurationSeconds) {
        return std::nullopt;
    }
    std::int64_t fraction = 0;
    if (dot != std::string_view::npos) {
        std::string digits(text.substr(dot + 1));
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
            return std::nullopt;
        }
        digits.resize(kFractionDigits, '0');
        fraction =
            static_cast<std::int64_t>(core::parse_integer<std::uint64_t>(digits).value_or(0));
    }
    return Micros{(static_cast<std::int64_t>(*whole) * kMicrosPerSecond) + fraction};
}

template <class T> bool take_number(std::string_view& text, std::size_t digits, T& out) {
    if (text.size() < digits) {
        return false;
    }
    const auto value = core::parse_integer<T>(text.substr(0, digits));
    if (!value) {
        return false;
    }
    out = *value;
    text.remove_prefix(digits);
    return true;
}

bool take(std::string_view& text, char c) {
    if (text.empty() || text.front() != c) {
        return false;
    }
    text.remove_prefix(1);
    return true;
}

// "2026-01-01T00:00:00.000Z" or the "+0000" / "+00:00" spellings of UTC, fraction optional.
// Any other offset is not a time this packager wrote or ffmpeg writes: no time, not an error.
std::optional<core::WallTime> parse_date_time(std::string_view text) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    if (!take_number(text, 4, year) || !take(text, '-') || !take_number(text, 2, month) ||
        !take(text, '-') || !take_number(text, 2, day) || !take(text, 'T') ||
        !take_number(text, 2, hour) || !take(text, ':') || !take_number(text, 2, minute) ||
        !take(text, ':') || !take_number(text, 2, second)) {
        return std::nullopt;
    }
    std::int64_t millis = 0;
    if (take(text, '.')) {
        const std::size_t end = text.find_first_not_of("0123456789");
        std::string digits(text.substr(0, end));
        if (digits.empty()) {
            return std::nullopt;
        }
        text.remove_prefix(digits.size());
        digits.resize(3, '0');
        millis = static_cast<std::int64_t>(core::parse_integer<std::uint64_t>(digits).value_or(0));
    }
    if (text != "Z" && text != "+0000" && text != "+00:00") {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{month},
                                           std::chrono::day{day}};
    if (!date.ok() || hour > 23 || minute > 59 || second > 60) {
        return std::nullopt;
    }
    const auto seconds = std::chrono::sys_days{date} + std::chrono::hours{hour} +
                         std::chrono::minutes{minute} + std::chrono::seconds{second};
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(seconds) +
           std::chrono::milliseconds{millis};
}

// The URI of `#EXT-X-MAP:URI="x",BYTERANGE=...`; nullopt when it has none or is unclosed.
std::optional<std::string> map_uri(std::string_view attributes) {
    constexpr std::string_view kUri = "URI=\"";
    const std::size_t at = attributes.find(kUri);
    if (at == std::string_view::npos || (at != 0 && attributes[at - 1] != ',')) {
        return std::nullopt;
    }
    const std::string_view rest = attributes.substr(at + kUri.size());
    const std::size_t close = rest.find('"');
    if (close == std::string_view::npos || close == 0) {
        return std::nullopt;
    }
    return std::string(rest.substr(0, close));
}

std::string format_date_time(core::WallTime t) {
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(t));
}

std::string format_duration(Micros d) {
    return std::format("{}.{:06}", d.count() / kMicrosPerSecond, d.count() % kMicrosPerSecond);
}

} // namespace

namespace {

template <class T> bool number_after(std::string_view line, std::string_view tag, T& out) {
    const auto value = core::parse_integer<T>(line.substr(tag.size()));
    if (value) {
        out = *value;
    }
    return value.has_value();
}

// The tags and segment lines after #EXTM3U, one at a time. A tag applies to the segment that
// follows it, so what is pending is held here until that segment's URI arrives.
class Parser {
public:
    [[nodiscard]] std::optional<PlaylistError> take(std::string_view line) {
        if (!line.starts_with('#')) {
            return segment(line);
        }
        if (line.starts_with(kTargetDuration)) {
            return malformed_unless(number_after(line, kTargetDuration, playlist_.target_seconds));
        }
        if (line.starts_with(kMediaSequence)) {
            return malformed_unless(number_after(line, kMediaSequence, playlist_.media_sequence));
        }
        if (line.starts_with(kDiscontinuitySequence)) {
            return malformed_unless(
                number_after(line, kDiscontinuitySequence, playlist_.discontinuity_sequence));
        }
        if (line == kDiscontinuity) {
            discontinuity_ = true;
        } else if (line.starts_with(kMap)) {
            auto uri = map_uri(line.substr(kMap.size()));
            if (!uri) {
                return PlaylistError::Malformed;
            }
            init_ = std::move(*uri);
        } else if (line.starts_with(kProgramDateTime)) {
            date_time_ = parse_date_time(line.substr(kProgramDateTime.size()));
        } else if (line.starts_with(kExtInf)) {
            const std::string_view value = line.substr(kExtInf.size());
            const auto duration = parse_duration(value.substr(0, value.find(',')));
            if (!duration || announced_) {
                return PlaylistError::Malformed;
            }
            announced_ = *duration;
        } else if (line == kEndList) {
            playlist_.ended = true;
        }
        return std::nullopt;
    }

    // Nothing may be left announced but never given a URI.
    [[nodiscard]] std::expected<MediaPlaylist, PlaylistError> finish() {
        if (announced_) {
            return std::unexpected(PlaylistError::Malformed);
        }
        return std::move(playlist_);
    }

private:
    static std::optional<PlaylistError> malformed_unless(bool ok) {
        return ok ? std::nullopt : std::optional<PlaylistError>(PlaylistError::Malformed);
    }

    std::optional<PlaylistError> segment(std::string_view uri) {
        if (!announced_) {
            return PlaylistError::Malformed;
        }
        if (playlist_.segments.size() == kMaxSegments) {
            return PlaylistError::TooLong;
        }
        playlist_.segments.push_back({.uri = std::string(uri),
                                      .duration = *announced_,
                                      .init = init_,
                                      .discontinuity = std::exchange(discontinuity_, false),
                                      .program_date_time = std::exchange(date_time_, {})});
        announced_.reset();
        return std::nullopt;
    }

    MediaPlaylist playlist_;
    std::string init_;
    std::optional<Micros> announced_;
    bool discontinuity_ = false;
    std::optional<core::WallTime> date_time_;
};

} // namespace

std::expected<MediaPlaylist, PlaylistError> parse_media_playlist(std::string_view text) {
    Parser parser;
    bool first = true;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (first) {
            if (line != "#EXTM3U") {
                return std::unexpected(PlaylistError::NotAPlaylist);
            }
            first = false;
        } else if (!line.empty()) {
            if (const auto error = parser.take(line)) {
                return std::unexpected(*error);
            }
        }
    }
    if (first) {
        return std::unexpected(PlaylistError::NotAPlaylist);
    }
    return parser.finish();
}

std::string render_media_playlist(const MediaPlaylist& playlist) {
    std::string out = "#EXTM3U\n#EXT-X-VERSION:7\n";
    out += std::format("#EXT-X-TARGETDURATION:{}\n#EXT-X-MEDIA-SEQUENCE:{}\n",
                       playlist.target_seconds, playlist.media_sequence);
    if (playlist.discontinuity_sequence != 0) {
        out += std::format("#EXT-X-DISCONTINUITY-SEQUENCE:{}\n", playlist.discontinuity_sequence);
    }
    out += "#EXT-X-INDEPENDENT-SEGMENTS\n";
    const std::string* init = nullptr;
    for (const Segment& segment : playlist.segments) {
        if (segment.discontinuity) {
            out += "#EXT-X-DISCONTINUITY\n";
        }
        if (!segment.init.empty() && (init == nullptr || *init != segment.init)) {
            out += std::format("#EXT-X-MAP:URI=\"{}\"\n", segment.init);
            init = &segment.init;
        }
        if (segment.program_date_time) {
            out += std::format("#EXT-X-PROGRAM-DATE-TIME:{}\n",
                               format_date_time(*segment.program_date_time));
        }
        out += std::format("#EXTINF:{},\n{}\n", format_duration(segment.duration), segment.uri);
    }
    if (playlist.ended) {
        out += "#EXT-X-ENDLIST\n";
    }
    return out;
}

} // namespace live

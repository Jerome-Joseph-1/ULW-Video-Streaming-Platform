#include "codec/sdp/parser.hpp"

#include "codec/sdp/error.hpp"
#include "codec/sdp/session.hpp"

#include "attributes.hpp"
#include "text.hpp"
#include "validate.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <vector>

namespace codec::sdp {
namespace {

using detail::canonical_number;
using detail::Fields;
using detail::is_non_ws_string;
using detail::is_token;
using detail::Level;

// An RTP section lists at most the 128 payload types there are; other protocols list one or
// two formats.
constexpr std::size_t kMaxFormats = 128;

struct Line {
    std::string_view text;
    std::uint32_t number;
};

// RFC 8866 section 5 fixes the order of lines. A rank per type; a line may not come after one
// of higher rank, nor repeat unless its type repeats.
enum class Rank : std::uint8_t { V, O, S, I, U, E, P, C, B, T, Z, K, A, M };

std::optional<Rank> session_rank(char type) noexcept {
    switch (type) {
    case 'v':
        return Rank::V;
    case 'o':
        return Rank::O;
    case 's':
        return Rank::S;
    case 'i':
        return Rank::I;
    case 'u':
        return Rank::U;
    case 'e':
        return Rank::E;
    case 'p':
        return Rank::P;
    case 'c':
        return Rank::C;
    case 'b':
        return Rank::B;
    case 't':
    case 'r':
        return Rank::T;
    case 'z':
        return Rank::Z;
    case 'k':
        return Rank::K;
    case 'a':
        return Rank::A;
    case 'm':
        return Rank::M;
    default:
        return std::nullopt;
    }
}

enum class MediaRank : std::uint8_t { M, I, C, B, K, A };

std::optional<MediaRank> media_rank(char type) noexcept {
    switch (type) {
    case 'i':
        return MediaRank::I;
    case 'c':
        return MediaRank::C;
    case 'b':
        return MediaRank::B;
    case 'k':
        return MediaRank::K;
    case 'a':
        return MediaRank::A;
    default:
        return std::nullopt;
    }
}

bool is_protocol(std::string_view s) noexcept {
    while (true) {
        const std::size_t slash = s.find('/');
        if (!is_token(s.substr(0, slash))) {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        s.remove_prefix(slash + 1);
    }
}

std::optional<Origin> origin(std::string_view v) noexcept {
    Fields f{v};
    const std::string_view username = f.next();
    const auto id = canonical_number<std::uint64_t>(f.next());
    const auto version = canonical_number<std::uint64_t>(f.next());
    Origin o{.username = username,
             .session_id = 0,
             .session_version = 0,
             .network_type = f.next(),
             .address_type = f.next(),
             .address = f.next()};
    if (!is_non_ws_string(username) || !id || !version || !is_token(o.network_type) ||
        !is_token(o.address_type) || !is_non_ws_string(o.address) || !f.done()) {
        return std::nullopt;
    }
    o.session_id = *id;
    o.session_version = *version;
    return o;
}

std::optional<Connection> connection(std::string_view v) noexcept {
    Fields f{v};
    const Connection c{.network_type = f.next(), .address_type = f.next(), .address = f.next()};
    if (!is_token(c.network_type) || !is_token(c.address_type) || !is_non_ws_string(c.address) ||
        !f.done()) {
        return std::nullopt;
    }
    return c;
}

std::optional<Bandwidth> bandwidth(std::string_view v) noexcept {
    const std::size_t colon = v.find(':');
    if (colon == std::string_view::npos || !is_token(v.substr(0, colon))) {
        return std::nullopt;
    }
    const auto value = canonical_number<std::uint64_t>(v.substr(colon + 1));
    if (!value) {
        return std::nullopt;
    }
    return Bandwidth{.type = v.substr(0, colon), .value = *value};
}

std::optional<Timing> timing(std::string_view v) {
    Fields f{v};
    const auto start = canonical_number<std::uint64_t>(f.next());
    const auto stop = canonical_number<std::uint64_t>(f.next());
    if (!start || !stop || !f.done()) {
        return std::nullopt;
    }
    return Timing{.start = *start, .stop = *stop, .repeats = {}};
}

std::expected<MediaDescription, ErrorCode> media_line(std::string_view v) {
    Fields f{v};
    MediaDescription m;
    m.media = f.next();
    std::string_view port = f.next();
    m.protocol = f.next();
    if (!is_token(m.media) || !is_protocol(m.protocol) || f.done()) {
        return std::unexpected{ErrorCode::BadMediaLine};
    }
    if (const std::size_t slash = port.find('/'); slash != std::string_view::npos) {
        m.port_count = canonical_number<std::uint16_t>(port.substr(slash + 1));
        if (!m.port_count || *m.port_count == 0) {
            return std::unexpected{ErrorCode::BadMediaLine};
        }
        port = port.substr(0, slash);
    }
    const auto number = canonical_number<std::uint16_t>(port);
    if (!number) {
        return std::unexpected{ErrorCode::BadMediaLine};
    }
    m.port = *number;
    const bool rtp = detail::carries_rtp(m.protocol);
    // RFC 3550 section 5.1: 7 bits.
    constexpr std::uint8_t kMaxPayloadType = 127;
    while (!f.done()) {
        const std::string_view format = f.next();
        if (!is_token(format)) {
            return std::unexpected{ErrorCode::BadMediaLine};
        }
        if (rtp) {
            const auto pt = canonical_number<std::uint8_t>(format);
            if (!pt || *pt > kMaxPayloadType) {
                return std::unexpected{ErrorCode::BadMediaLine};
            }
        }
        if (m.formats.size() == kMaxFormats) {
            return std::unexpected{ErrorCode::TooManyFormats};
        }
        if (std::ranges::contains(m.formats, format)) {
            return std::unexpected{ErrorCode::DuplicateFormat};
        }
        m.formats.push_back(format);
    }
    return m;
}

struct Typed {
    char type;
    std::string_view value;
    std::uint32_t number;
};

class Parser {
public:
    Parser(std::string_view text, const Limits& limits) noexcept : rest_(text), limits_(limits) {}

    std::expected<Session, Error> run() {
        while (const std::optional<Line> line = next()) {
            if (const auto code = take(*line)) {
                return std::unexpected{Error{.code = *code, .line = line->number}};
            }
        }
        if (const auto code = missing_before(Rank::M)) {
            return std::unexpected{Error{.code = *code, .line = number_ + 1}};
        }
        if (const auto error = detail::validate(session_)) {
            return std::unexpected{*error};
        }
        return std::move(session_);
    }

private:
    // Lines end in LF or CRLF.
    std::optional<Line> next() noexcept {
        if (rest_.empty()) {
            return std::nullopt;
        }
        const std::size_t lf = rest_.find('\n');
        std::string_view text = rest_.substr(0, lf);
        rest_ = lf == std::string_view::npos ? std::string_view{} : rest_.substr(lf + 1);
        if (lf != std::string_view::npos && text.ends_with('\r')) {
            text.remove_suffix(1);
        }
        ++number_;
        return Line{.text = text, .number = number_};
    }

    [[nodiscard]] std::optional<ErrorCode> missing_before(Rank rank) const noexcept {
        if (!seen_version_) {
            return ErrorCode::MissingLine;
        }
        if ((rank > Rank::O && !seen_origin_) || (rank > Rank::S && !seen_name_) ||
            (rank > Rank::T && session_.timings.empty())) {
            return ErrorCode::MissingLine;
        }
        return std::nullopt;
    }

    std::optional<ErrorCode> take(const Line& raw) {
        // A CR anywhere but before the LF, or a NUL, is outside RFC 8866's byte-string.
        if (raw.text.find_first_of(std::string_view{"\r\0", 2}) != std::string_view::npos) {
            return ErrorCode::ForbiddenCharacter;
        }
        if (raw.text.size() < 2 || raw.text[1] != '=' || raw.text[0] < 'a' || raw.text[0] > 'z') {
            return ErrorCode::MalformedLine;
        }
        const Typed line{.type = raw.text[0], .value = raw.text.substr(2), .number = raw.number};
        if (!session_.media.empty() && line.type != 'm') {
            return take_media(line);
        }
        const std::optional<Rank> rank = session_rank(line.type);
        if (!rank) {
            return ErrorCode::UnknownLineType;
        }
        if (line.number == 1 && line.type != 'v') {
            return ErrorCode::MissingLine;
        }
        if (*rank != Rank::V) {
            if (const auto code = missing_before(*rank)) {
                return code;
            }
        }
        if (rank_ && (*rank < *rank_ || (*rank == *rank_ && !repeats(line.type)))) {
            return ErrorCode::MisplacedLine;
        }
        if (line.type == 'r' && previous_ != 't' && previous_ != 'r') {
            return ErrorCode::MisplacedLine;
        }
        rank_ = rank;
        previous_ = line.type;
        return take_session(line);
    }

    static bool repeats(char type) noexcept {
        return type == 'e' || type == 'p' || type == 'b' || type == 't' || type == 'r' ||
               type == 'a' || type == 'm';
    }

    std::optional<ErrorCode> take_session(const Typed& line) {
        const std::string_view v = line.value;
        Session& s = session_;
        switch (line.type) {
        case 'v':
            seen_version_ = true;
            return v == "0" ? std::nullopt : std::optional{ErrorCode::BadVersion};
        case 'o':
            if (const auto o = origin(v)) {
                s.origin = *o;
                seen_origin_ = true;
                return std::nullopt;
            }
            return ErrorCode::BadOrigin;
        case 's':
            if (v.empty()) {
                return ErrorCode::BadSessionName;
            }
            s.name = v;
            seen_name_ = true;
            return std::nullopt;
        case 'i':
            return text_field(v, s.information);
        case 'u':
            return text_field(v, s.uri);
        case 'e':
            return text_field(v, s.emails);
        case 'p':
            return text_field(v, s.phones);
        case 'c':
            if (const auto c = connection(v)) {
                s.connection = *c;
                return std::nullopt;
            }
            return ErrorCode::BadConnection;
        case 'b':
            return take_bandwidth(v, s.bandwidths);
        case 't':
            if (auto t = timing(v)) {
                s.timings.push_back(std::move(*t));
                return std::nullopt;
            }
            return ErrorCode::BadTiming;
        case 'r':
            return text_field(v, s.timings.back().repeats);
        case 'z':
            return text_field(v, s.zone_adjustments);
        case 'k':
            return text_field(v, s.key);
        case 'a':
            return take_attribute(line, Level::Session, s.attributes);
        case 'm':
            return take_media_line(line);
        default:
            return ErrorCode::UnknownLineType;
        }
    }

    std::optional<ErrorCode> take_media_line(const Typed& line) {
        if (session_.media.size() == limits_.max_media_sections) {
            return ErrorCode::TooManyMediaSections;
        }
        auto m = media_line(line.value);
        if (!m) {
            return m.error();
        }
        m->line = line.number;
        session_.media.push_back(std::move(*m));
        media_rank_ = MediaRank::M;
        return std::nullopt;
    }

    std::optional<ErrorCode> take_media(const Typed& line) {
        const std::optional<MediaRank> rank = media_rank(line.type);
        if (!rank) {
            return session_rank(line.type) ? ErrorCode::MisplacedLine : ErrorCode::UnknownLineType;
        }
        const bool repeatable = line.type == 'c' || line.type == 'b' || line.type == 'a';
        if (*rank < media_rank_ || (*rank == media_rank_ && !repeatable)) {
            return ErrorCode::MisplacedLine;
        }
        media_rank_ = *rank;
        MediaDescription& m = session_.media.back();
        const std::string_view v = line.value;
        switch (*rank) {
        case MediaRank::I:
            return text_field(v, m.information);
        case MediaRank::C:
            if (const auto c = connection(v)) {
                m.connections.push_back(*c);
                return std::nullopt;
            }
            return ErrorCode::BadConnection;
        case MediaRank::B:
            return take_bandwidth(v, m.bandwidths);
        case MediaRank::K:
            return text_field(v, m.key);
        case MediaRank::A:
            return take_attribute(line, Level::Media, m.attributes);
        case MediaRank::M:
            break;
        }
        return ErrorCode::MisplacedLine;
    }

    static std::optional<ErrorCode> take_bandwidth(std::string_view v, std::vector<Bandwidth>& to) {
        if (const auto b = bandwidth(v)) {
            to.push_back(*b);
            return std::nullopt;
        }
        return ErrorCode::BadBandwidth;
    }

    // i=, u=, e=, p=, r=, z= and k= are kept verbatim; RFC 8866 only requires them non-empty.
    static std::optional<ErrorCode> text_field(std::string_view v,
                                               std::optional<std::string_view>& to) noexcept {
        if (v.empty()) {
            return ErrorCode::MalformedLine;
        }
        to = v;
        return std::nullopt;
    }

    static std::optional<ErrorCode> text_field(std::string_view v,
                                               std::vector<std::string_view>& to) {
        if (v.empty()) {
            return ErrorCode::MalformedLine;
        }
        to.push_back(v);
        return std::nullopt;
    }

    std::optional<ErrorCode> take_attribute(const Typed& line, Level level,
                                            std::vector<Attribute>& to) const {
        if (to.size() == limits_.max_attributes) {
            return ErrorCode::TooManyAttributes;
        }
        const std::size_t colon = line.value.find(':');
        const std::string_view name = line.value.substr(0, colon);
        std::optional<std::string_view> value;
        if (colon != std::string_view::npos) {
            value = line.value.substr(colon + 1);
        }
        if (!is_token(name)) {
            return ErrorCode::BadAttribute;
        }
        std::optional<AttributeValue> parsed = detail::parse_attribute(name, value);
        if (!parsed) {
            return ErrorCode::BadAttribute;
        }
        if (!detail::allowed_at(*parsed, level)) {
            return ErrorCode::MisplacedAttribute;
        }
        to.push_back(Attribute{.value = std::move(*parsed), .line = line.number});
        return std::nullopt;
    }

    std::string_view rest_;
    const Limits& limits_;
    std::uint32_t number_ = 0;
    Session session_;
    std::optional<Rank> rank_;
    char previous_ = '\0';
    MediaRank media_rank_ = MediaRank::M;
    bool seen_version_ = false;
    bool seen_origin_ = false;
    bool seen_name_ = false;
};

} // namespace

std::expected<Session, Error> parse(std::string_view text, const Limits& limits) {
    if (text.size() > limits.max_bytes) {
        return std::unexpected{Error{.code = ErrorCode::TooLarge, .line = 0}};
    }
    return Parser{text, limits}.run();
}

} // namespace codec::sdp

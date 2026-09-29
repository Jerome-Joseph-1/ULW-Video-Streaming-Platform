#include "attributes.hpp"

#include "codec/sdp/session.hpp"

#include "text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace codec::sdp::detail {
namespace {

template <typename... F> struct Overloaded : F... {
    using F::operator()...;
};

// RTP payload types are 7 bits (RFC 3550 section 5.1).
constexpr std::uint8_t kMaxPayloadType = 127;

std::optional<std::uint8_t> payload_type(std::string_view s) noexcept {
    const auto pt = canonical_number<std::uint8_t>(s);
    if (!pt || *pt > kMaxPayloadType) {
        return std::nullopt;
    }
    return pt;
}

// RFC 8839 section 5.1: ice-char = ALPHA / DIGIT / "+" / "/".
bool is_ice_chars(std::string_view s, std::size_t min, std::size_t max) noexcept {
    return s.size() >= min && s.size() <= max && std::ranges::all_of(s, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '+' || c == '/';
           });
}

// RFC 8851 section 10: rid-id = 1*(alpha-numeric / "-" / "_").
bool is_rid_id(std::string_view s) noexcept {
    return !s.empty() && std::ranges::all_of(s, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
    });
}

bool is_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

// RFC 8122 section 5: 2UHEX *(":" 2UHEX), upper case. Chromium accepts lower case too, and the
// value is kept as written, so accepting it costs the round trip nothing.
bool is_fingerprint_value(std::string_view s) noexcept {
    constexpr std::size_t kPair = 3;
    if (s.size() % kPair != 2) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i % kPair == 2) {
            if (c != ':') {
                return false;
            }
        } else if (!is_hex(c)) {
            return false;
        }
    }
    return true;
}

std::optional<std::string_view> remainder(const Fields& f) noexcept {
    if (f.done()) {
        return std::nullopt;
    }
    return f.rest();
}

std::optional<Direction> direction(std::string_view s) noexcept {
    if (s == "sendrecv") {
        return Direction::SendRecv;
    }
    if (s == "sendonly") {
        return Direction::SendOnly;
    }
    if (s == "recvonly") {
        return Direction::RecvOnly;
    }
    if (s == "inactive") {
        return Direction::Inactive;
    }
    return std::nullopt;
}

std::optional<RidDirection> rid_direction(std::string_view s) noexcept {
    if (s == "send") {
        return RidDirection::Send;
    }
    if (s == "recv") {
        return RidDirection::Recv;
    }
    return std::nullopt;
}

std::optional<AttributeValue> rtpmap(std::string_view v) {
    Fields f{v};
    const auto pt = payload_type(f.next());
    const std::string_view encoding = f.next();
    if (!pt || !f.done()) {
        return std::nullopt;
    }
    // <encoding name>/<clock rate>[/<encoding parameters>]
    const std::size_t first = encoding.find('/');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view name = encoding.substr(0, first);
    const std::string_view rest = encoding.substr(first + 1);
    const std::size_t second = rest.find('/');
    const auto clock = canonical_number<std::uint32_t>(rest.substr(0, second));
    if (!is_token(name) || !clock || *clock == 0) {
        return std::nullopt;
    }
    Rtpmap r{.payload_type = *pt, .encoding = name, .clock_rate = *clock, .channels = {}};
    if (second != std::string_view::npos) {
        r.channels = canonical_number<std::uint32_t>(rest.substr(second + 1));
        if (!r.channels || *r.channels == 0) {
            return std::nullopt;
        }
    }
    return r;
}

std::optional<AttributeValue> fmtp(std::string_view v) {
    Fields f{v};
    const std::string_view format = f.next();
    const auto parameters = remainder(f);
    if (!is_token(format) || !parameters || parameters->empty()) {
        return std::nullopt;
    }
    return Fmtp{.format = format, .parameters = *parameters};
}

std::optional<AttributeValue> rtcp_fb(std::string_view v) {
    Fields f{v};
    const std::string_view pt_text = f.next();
    RtcpFeedback fb;
    if (pt_text != "*") {
        const auto pt = payload_type(pt_text);
        if (!pt) {
            return std::nullopt;
        }
        fb.payload_type = pt;
    }
    fb.type = f.next();
    fb.parameter = remainder(f);
    if (!is_token(fb.type) || (fb.parameter && fb.parameter->empty())) {
        return std::nullopt;
    }
    return fb;
}

std::optional<AttributeValue> extmap(std::string_view v) {
    Fields f{v};
    std::string_view id_text = f.next();
    Extmap e;
    if (const std::size_t slash = id_text.find('/'); slash != std::string_view::npos) {
        e.direction = direction(id_text.substr(slash + 1));
        if (!e.direction) {
            return std::nullopt;
        }
        id_text = id_text.substr(0, slash);
    }
    const auto id = canonical_number<std::uint16_t>(id_text);
    // RFC 8285 section 5 numbers extensions 1-255; section 7 reserves 4096-4351 for an offer
    // that lists more extensions than the answerer can number.
    constexpr std::uint16_t kMaxId = 255;
    constexpr std::uint16_t kOfferFirst = 4096;
    constexpr std::uint16_t kOfferLast = 4351;
    if (!id || *id == 0 || (*id > kMaxId && (*id < kOfferFirst || *id > kOfferLast))) {
        return std::nullopt;
    }
    e.id = *id;
    e.uri = f.next();
    e.attributes = remainder(f);
    if (!is_non_ws_string(e.uri) || (e.attributes && e.attributes->empty())) {
        return std::nullopt;
    }
    return e;
}

std::optional<AttributeValue> group(std::string_view v) {
    Fields f{v};
    Group g{.semantics = f.next(), .tags = {}};
    if (!is_token(g.semantics)) {
        return std::nullopt;
    }
    while (!f.done()) {
        const std::string_view tag = f.next();
        if (!is_token(tag)) {
            return std::nullopt;
        }
        g.tags.push_back(tag);
    }
    return g;
}

// RFC 8830 section 2: msid-id and msid-appdata are 1*64token-char.
bool is_msid_part(std::string_view s) noexcept {
    constexpr std::size_t kMax = 64;
    return is_token(s) && s.size() <= kMax;
}

std::optional<AttributeValue> msid(std::string_view v) {
    Fields f{v};
    Msid m{.stream = f.next(), .track = {}};
    if (!f.done()) {
        m.track = f.next();
    }
    if (!is_msid_part(m.stream) || (m.track && !is_msid_part(*m.track)) || !f.done()) {
        return std::nullopt;
    }
    return m;
}

std::optional<AttributeValue> ssrc(std::string_view v) {
    Fields f{v};
    const auto id = canonical_number<std::uint32_t>(f.next());
    const auto rest = remainder(f);
    if (!id || !rest) {
        return std::nullopt;
    }
    Ssrc s{.ssrc = *id, .attribute = *rest, .value = {}};
    if (const std::size_t colon = rest->find(':'); colon != std::string_view::npos) {
        s.attribute = rest->substr(0, colon);
        s.value = rest->substr(colon + 1);
    }
    if (!is_token(s.attribute)) {
        return std::nullopt;
    }
    return s;
}

std::optional<AttributeValue> ssrc_group(std::string_view v) {
    Fields f{v};
    SsrcGroup g{.semantics = f.next(), .ssrcs = {}};
    if (!is_token(g.semantics) || f.done()) {
        return std::nullopt;
    }
    while (!f.done()) {
        const auto id = canonical_number<std::uint32_t>(f.next());
        if (!id) {
            return std::nullopt;
        }
        g.ssrcs.push_back(*id);
    }
    return g;
}

std::optional<AttributeValue> ice_options(std::string_view v) {
    Fields f{v};
    IceOptions o;
    while (!f.done()) {
        const std::string_view option = f.next();
        // RFC 8839 section 5.6: ice-option-tag = 1*ice-char, no upper bound.
        if (!is_ice_chars(option, 1, option.size())) {
            return std::nullopt;
        }
        o.options.push_back(option);
    }
    return o;
}

std::optional<AttributeValue> candidate(std::string_view v) {
    Fields f{v};
    Candidate c;
    c.foundation = f.next();
    const auto component = canonical_number<std::uint16_t>(f.next());
    c.transport = f.next();
    const auto priority = canonical_number<std::uint32_t>(f.next());
    c.address = f.next();
    const auto port = canonical_number<std::uint16_t>(f.next());
    const std::string_view typ = f.next();
    c.type = f.next();
    // RFC 8839 section 5.1: foundation is 1*32ice-char, component-id 1*3DIGIT (1 to 256 in
    // RFC 8445 section 5.1.2.1), priority 1*10DIGIT (1 to 2^31 - 1).
    constexpr std::size_t kMaxFoundation = 32;
    constexpr std::uint16_t kMaxComponent = 256;
    constexpr std::uint32_t kMaxPriority = 0x7FFF'FFFF;
    if (!is_ice_chars(c.foundation, 1, kMaxFoundation) || !component || *component == 0 ||
        *component > kMaxComponent || !is_token(c.transport) || !priority || *priority == 0 ||
        *priority > kMaxPriority || !is_non_ws_string(c.address) || !port || typ != "typ" ||
        !is_token(c.type)) {
        return std::nullopt;
    }
    c.component = *component;
    c.priority = *priority;
    c.port = *port;
    Fields before = f;
    std::string_view field = f.next();
    if (!before.done() && field == "raddr") {
        c.related_address = f.next();
        if (!is_non_ws_string(*c.related_address)) {
            return std::nullopt;
        }
        before = f;
        field = f.next();
    }
    if (!before.done() && field == "rport") {
        c.related_port = canonical_number<std::uint16_t>(f.next());
        if (!c.related_port) {
            return std::nullopt;
        }
        before = f;
    }
    c.extensions = remainder(before);
    if (c.extensions && c.extensions->empty()) {
        return std::nullopt;
    }
    return c;
}

std::optional<AttributeValue> fingerprint(std::string_view v) {
    Fields f{v};
    Fingerprint fp{.algorithm = f.next(), .value = f.next()};
    if (!is_token(fp.algorithm) || !is_fingerprint_value(fp.value) || !f.done()) {
        return std::nullopt;
    }
    return fp;
}

std::optional<AttributeValue> setup(std::string_view v) {
    if (v == "active") {
        return Setup{SetupRole::Active};
    }
    if (v == "passive") {
        return Setup{SetupRole::Passive};
    }
    if (v == "actpass") {
        return Setup{SetupRole::ActPass};
    }
    if (v == "holdconn") {
        return Setup{SetupRole::HoldConn};
    }
    return std::nullopt;
}

std::optional<AttributeValue> rid(std::string_view v) {
    Fields f{v};
    const std::string_view id = f.next();
    const auto dir = rid_direction(f.next());
    const auto restrictions = remainder(f);
    if (!is_rid_id(id) || !dir || (restrictions && restrictions->empty())) {
        return std::nullopt;
    }
    return Rid{.id = id, .direction = *dir, .restrictions = restrictions};
}

// RFC 8853 section 5.1: sc-str-list = sc-alt-list *(";" sc-alt-list), sc-alt-list = sc-id
// *("," sc-id), sc-id = [sc-id-paused] rid-id.
bool is_simulcast_list(std::string_view s) noexcept {
    const auto each = [](std::string_view list, char sep, auto&& ok) {
        while (true) {
            const std::size_t at = list.find(sep);
            if (!ok(list.substr(0, at))) {
                return false;
            }
            if (at == std::string_view::npos) {
                return true;
            }
            list.remove_prefix(at + 1);
        }
    };
    return each(s, ';', [&](std::string_view alternatives) {
        return each(alternatives, ',', [](std::string_view id) {
            if (id.starts_with('~')) {
                id.remove_prefix(1);
            }
            return is_rid_id(id);
        });
    });
}

std::optional<SimulcastStreams> simulcast_streams(Fields& f) noexcept {
    const auto dir = rid_direction(f.next());
    const std::string_view streams = f.next();
    if (!dir || !is_simulcast_list(streams)) {
        return std::nullopt;
    }
    return SimulcastStreams{.direction = *dir, .streams = streams};
}

std::optional<AttributeValue> simulcast(std::string_view v) {
    Fields f{v};
    const auto first = simulcast_streams(f);
    if (!first) {
        return std::nullopt;
    }
    Simulcast s{.first = *first, .second = {}};
    if (!f.done()) {
        s.second = simulcast_streams(f);
        if (!s.second || s.second->direction == first->direction) {
            return std::nullopt;
        }
    }
    if (!f.done()) {
        return std::nullopt;
    }
    return s;
}

std::optional<AttributeValue> flag(std::optional<std::string_view> value, AttributeValue typed) {
    if (value) {
        return std::nullopt;
    }
    return typed;
}

using ValueParser = std::optional<AttributeValue> (*)(std::string_view);

struct ValueAttribute {
    std::string_view name;
    ValueParser parse;
};

constexpr std::array kValueAttributes{
    ValueAttribute{.name = "rtpmap", .parse = rtpmap},
    ValueAttribute{.name = "fmtp", .parse = fmtp},
    ValueAttribute{.name = "rtcp-fb", .parse = rtcp_fb},
    ValueAttribute{.name = "extmap", .parse = extmap},
    ValueAttribute{.name = "mid",
                   .parse = [](std::string_view v) -> std::optional<AttributeValue> {
                       if (!is_token(v)) {
                           return std::nullopt;
                       }
                       return Mid{v};
                   }},
    ValueAttribute{.name = "group", .parse = group},
    ValueAttribute{.name = "msid", .parse = msid},
    ValueAttribute{.name = "ssrc", .parse = ssrc},
    ValueAttribute{.name = "ssrc-group", .parse = ssrc_group},
    // RFC 8839 section 5.4: ice-ufrag is 4 to 256 ice-chars, ice-pwd 22 to 256.
    ValueAttribute{.name = "ice-ufrag",
                   .parse = [](std::string_view v) -> std::optional<AttributeValue> {
                       if (!is_ice_chars(v, 4, 256)) {
                           return std::nullopt;
                       }
                       return IceUfrag{v};
                   }},
    ValueAttribute{.name = "ice-pwd",
                   .parse = [](std::string_view v) -> std::optional<AttributeValue> {
                       if (!is_ice_chars(v, 22, 256)) {
                           return std::nullopt;
                       }
                       return IcePwd{v};
                   }},
    ValueAttribute{.name = "ice-options", .parse = ice_options},
    ValueAttribute{.name = "candidate", .parse = candidate},
    ValueAttribute{.name = "fingerprint", .parse = fingerprint},
    ValueAttribute{.name = "setup", .parse = setup},
    ValueAttribute{.name = "rid", .parse = rid},
    ValueAttribute{.name = "simulcast", .parse = simulcast},
};

} // namespace

std::optional<AttributeValue> parse_attribute(std::string_view name,
                                              std::optional<std::string_view> value) {
    for (const ValueAttribute& a : kValueAttributes) {
        if (a.name == name) {
            if (!value) {
                return std::nullopt;
            }
            return a.parse(*value);
        }
    }
    if (name == "end-of-candidates") {
        return flag(value, EndOfCandidates{});
    }
    if (name == "rtcp-mux") {
        return flag(value, RtcpMux{});
    }
    if (name == "rtcp-rsize") {
        return flag(value, RtcpRsize{});
    }
    if (const auto d = direction(name)) {
        return flag(value, *d);
    }
    return UnknownAttribute{.name = name, .value = value};
}

bool allowed_at(const AttributeValue& value, Level level) {
    enum class Where : std::uint8_t { Session, Media, Both };
    const Where where = std::visit(Overloaded{
                                       [](const Group&) { return Where::Session; },
                                       [](const Rtpmap&) { return Where::Media; },
                                       [](const Fmtp&) { return Where::Media; },
                                       [](const RtcpFeedback&) { return Where::Media; },
                                       [](const Mid&) { return Where::Media; },
                                       [](const Msid&) { return Where::Media; },
                                       [](const Ssrc&) { return Where::Media; },
                                       [](const SsrcGroup&) { return Where::Media; },
                                       [](const Candidate&) { return Where::Media; },
                                       [](const RtcpMux&) { return Where::Media; },
                                       [](const RtcpRsize&) { return Where::Media; },
                                       [](const Rid&) { return Where::Media; },
                                       [](const Simulcast&) { return Where::Media; },
                                       [](const auto&) { return Where::Both; },
                                   },
                                   value);
    switch (where) {
    case Where::Session:
        return level == Level::Session;
    case Where::Media:
        return level == Level::Media;
    case Where::Both:
        return true;
    }
    return false;
}

std::string_view direction_name(Direction d) noexcept {
    switch (d) {
    case Direction::SendRecv:
        return "sendrecv";
    case Direction::SendOnly:
        return "sendonly";
    case Direction::RecvOnly:
        return "recvonly";
    case Direction::Inactive:
        return "inactive";
    }
    return {};
}

std::string_view setup_name(SetupRole role) noexcept {
    switch (role) {
    case SetupRole::Active:
        return "active";
    case SetupRole::Passive:
        return "passive";
    case SetupRole::ActPass:
        return "actpass";
    case SetupRole::HoldConn:
        return "holdconn";
    }
    return {};
}

std::string_view rid_direction_name(RidDirection d) noexcept {
    switch (d) {
    case RidDirection::Send:
        return "send";
    case RidDirection::Recv:
        return "recv";
    }
    return {};
}

std::string_view attribute_name(const AttributeValue& value) {
    return std::visit(
        Overloaded{
            [](const UnknownAttribute& a) { return a.name; },
            [](const Rtpmap&) { return std::string_view{"rtpmap"}; },
            [](const Fmtp&) { return std::string_view{"fmtp"}; },
            [](const RtcpFeedback&) { return std::string_view{"rtcp-fb"}; },
            [](const Extmap&) { return std::string_view{"extmap"}; },
            [](const Mid&) { return std::string_view{"mid"}; },
            [](const Group&) { return std::string_view{"group"}; },
            [](const Msid&) { return std::string_view{"msid"}; },
            [](const Ssrc&) { return std::string_view{"ssrc"}; },
            [](const SsrcGroup&) { return std::string_view{"ssrc-group"}; },
            [](const IceUfrag&) { return std::string_view{"ice-ufrag"}; },
            [](const IcePwd&) { return std::string_view{"ice-pwd"}; },
            [](const IceOptions&) { return std::string_view{"ice-options"}; },
            [](const Candidate&) { return std::string_view{"candidate"}; },
            [](const EndOfCandidates&) { return std::string_view{"end-of-candidates"}; },
            [](const Fingerprint&) { return std::string_view{"fingerprint"}; },
            [](const Setup&) { return std::string_view{"setup"}; },
            [](const RtcpMux&) { return std::string_view{"rtcp-mux"}; },
            [](const RtcpRsize&) { return std::string_view{"rtcp-rsize"}; },
            [](Direction d) { return direction_name(d); },
            [](const Rid&) { return std::string_view{"rid"}; },
            [](const Simulcast&) { return std::string_view{"simulcast"}; },
        },
        value);
}

} // namespace codec::sdp::detail

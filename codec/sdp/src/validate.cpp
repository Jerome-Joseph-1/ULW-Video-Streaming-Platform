#include "validate.hpp"

#include "codec/sdp/error.hpp"
#include "codec/sdp/session.hpp"

#include "text.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace codec::sdp::detail {
namespace {

// RFC 3550 section 5.1: 7-bit payload types.
constexpr std::size_t kPayloadTypes = 128;

template <typename T> const Attribute* find(std::span<const Attribute> attributes) noexcept {
    const auto it = std::ranges::find_if(
        attributes, [](const Attribute& a) { return std::holds_alternative<T>(a.value); });
    return it == attributes.end() ? nullptr : &*it;
}

bool is_bundle_only(const MediaDescription& m) noexcept {
    return std::ranges::any_of(m.attributes, [](const Attribute& a) {
        const auto* u = std::get_if<UnknownAttribute>(&a.value);
        return u != nullptr && u->name == "bundle-only" && !u->value;
    });
}

bool equals_lower(std::string_view s, std::string_view lower) noexcept {
    return std::ranges::equal(
        s, lower, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

// RFC 8122 section 5 allows sha-1 and MD5 as well; RFC 8827 section 6.5 requires SHA-256 of
// WebRTC endpoints, and neither of the older two is collision resistant any more.
std::optional<ErrorCode> check_fingerprint(const Fingerprint& fp) noexcept {
    struct Hash {
        std::string_view name;
        std::size_t bytes;
    };
    constexpr std::array kHashes{Hash{.name = "sha-256", .bytes = 32},
                                 Hash{.name = "sha-384", .bytes = 48},
                                 Hash{.name = "sha-512", .bytes = 64}};
    const auto* hash = std::ranges::find_if(
        kHashes, [&](const Hash& h) { return equals_lower(fp.algorithm, h.name); });
    if (hash == kHashes.end()) {
        return ErrorCode::UnsupportedFingerprintAlgorithm;
    }
    // "AB:CD:EF": three characters per byte, less the missing final colon.
    if ((fp.value.size() + 1) / 3 != hash->bytes) {
        return ErrorCode::BadFingerprint;
    }
    return std::nullopt;
}

std::optional<Error> check_fingerprints(std::span<const Attribute> attributes) noexcept {
    for (const Attribute& a : attributes) {
        if (const auto* fp = std::get_if<Fingerprint>(&a.value)) {
            if (const auto code = check_fingerprint(*fp)) {
                return Error{.code = *code, .line = a.line};
            }
        }
    }
    return std::nullopt;
}

// RFC 8285 section 5: an id names one extension across the session and the section.
std::optional<Error> check_extmap_ids(std::span<const Attribute> session,
                                      std::span<const Attribute> media) {
    std::vector<std::uint16_t> ids;
    for (const std::span<const Attribute> level : {session, media}) {
        for (const Attribute& a : level) {
            if (const auto* e = std::get_if<Extmap>(&a.value)) {
                if (std::ranges::contains(ids, e->id)) {
                    return Error{.code = ErrorCode::DuplicateExtmapId, .line = a.line};
                }
                ids.push_back(e->id);
            }
        }
    }
    return std::nullopt;
}

// JSEP (RFC 9429) section 5.8: rtpmap, fmtp and rtcp-fb describe formats of the m= line, and
// one payload type has one mapping.
std::optional<Error> check_formats(const MediaDescription& m) {
    std::bitset<kPayloadTypes> listed;
    if (carries_rtp(m.protocol)) {
        for (const std::string_view f : m.formats) {
            // The parser accepted only payload types on an RTP m= line.
            listed.set(canonical_number<std::uint8_t>(f).value_or(0));
        }
    }
    std::bitset<kPayloadTypes> mapped;
    for (const Attribute& a : m.attributes) {
        const Error unknown{.code = ErrorCode::UnknownPayloadType, .line = a.line};
        if (const auto* r = std::get_if<Rtpmap>(&a.value)) {
            if (!listed.test(r->payload_type)) {
                return unknown;
            }
            if (mapped.test(r->payload_type)) {
                return Error{.code = ErrorCode::DuplicatePayloadType, .line = a.line};
            }
            mapped.set(r->payload_type);
        } else if (const auto* f = std::get_if<Fmtp>(&a.value)) {
            if (!std::ranges::contains(m.formats, f->format)) {
                return unknown;
            }
        } else if (const auto* fb = std::get_if<RtcpFeedback>(&a.value)) {
            if (fb->payload_type && !listed.test(*fb->payload_type)) {
                return unknown;
            }
        }
    }
    return std::nullopt;
}

// RFC 8853 section 5.2: every rid a=simulcast names is declared by an a=rid of the same
// direction in the section.
bool declared(const MediaDescription& m, const SimulcastStreams& streams) {
    // The parser has checked the list's grammar; only the ids are left to look up.
    std::string_view list = streams.streams;
    while (!list.empty()) {
        const std::size_t end = list.find_first_of(",;");
        std::string_view id = list.substr(0, end);
        list = end == std::string_view::npos ? std::string_view{} : list.substr(end + 1);
        if (id.starts_with('~')) {
            id.remove_prefix(1);
        }
        const bool found = std::ranges::any_of(m.attributes, [&](const Attribute& a) {
            const auto* r = std::get_if<Rid>(&a.value);
            return r != nullptr && r->id == id && r->direction == streams.direction;
        });
        if (!found) {
            return false;
        }
    }
    return true;
}

std::optional<Error> check_simulcast(const MediaDescription& m) {
    for (const Attribute& a : m.attributes) {
        const auto* s = std::get_if<Simulcast>(&a.value);
        if (s != nullptr && (!declared(m, s->first) || (s->second && !declared(m, *s->second)))) {
            return Error{.code = ErrorCode::UnknownRid, .line = a.line};
        }
    }
    return std::nullopt;
}

template <typename T> bool described(const Session& s, const MediaDescription& m) noexcept {
    return find<T>(m.attributes) != nullptr || find<T>(s.attributes) != nullptr;
}

// Asked of a section outside any BUNDLE group, or of the tagged (first) section of a group,
// from which the others take their transport (RFC 9143 section 7).
std::optional<Error> check_transport(const Session& s, const MediaDescription& m) noexcept {
    if (!described<IceUfrag>(s, m) || !described<IcePwd>(s, m)) {
        return Error{.code = ErrorCode::MissingIceCredentials, .line = m.line};
    }
    if (!described<Fingerprint>(s, m)) {
        return Error{.code = ErrorCode::MissingFingerprint, .line = m.line};
    }
    if (!described<Setup>(s, m)) {
        return Error{.code = ErrorCode::MissingSetup, .line = m.line};
    }
    return std::nullopt;
}

struct Bundling {
    // Per section: in a BUNDLE group, and whether it is the group's tagged section.
    std::vector<bool> bundled;
    std::vector<bool> tagged;
};

// RFC 9143 sections 7 and 8: a group names sections that exist, a section is in at most one
// group, and the tagged section is not one the offerer asks to be accepted only if bundled.
std::optional<Error> check_bundle(const Session& s, const std::vector<std::string_view>& mids,
                                  Bundling& out) {
    out.bundled.assign(s.media.size(), false);
    out.tagged.assign(s.media.size(), false);
    for (const Attribute& a : s.attributes) {
        const auto* g = std::get_if<Group>(&a.value);
        if (g == nullptr || g->semantics != "BUNDLE") {
            continue;
        }
        const Error bad{.code = ErrorCode::BadBundleGroup, .line = a.line};
        for (const std::string_view tag : g->tags) {
            const auto it = std::ranges::find(mids, tag);
            if (it == mids.end()) {
                return bad;
            }
            const auto i = static_cast<std::size_t>(it - mids.begin());
            if (out.bundled[i]) {
                return bad;
            }
            out.bundled[i] = true;
            if (tag == g->tags.front()) {
                const MediaDescription& tagged = s.media[i];
                if (tagged.port == 0 || is_bundle_only(tagged)) {
                    return bad;
                }
                out.tagged[i] = true;
            }
        }
    }
    for (std::size_t i = 0; i < s.media.size(); ++i) {
        if (!out.bundled[i] && is_bundle_only(s.media[i])) {
            return Error{.code = ErrorCode::BadBundleGroup, .line = s.media[i].line};
        }
    }
    return std::nullopt;
}

// JSEP section 5.2.1: every m= section carries exactly one mid, unique in the description.
std::optional<Error> collect_mids(const Session& s, std::vector<std::string_view>& mids) {
    for (const MediaDescription& m : s.media) {
        const Mid* mid = nullptr;
        for (const Attribute& a : m.attributes) {
            if (const auto* found = std::get_if<Mid>(&a.value)) {
                if (mid != nullptr || std::ranges::contains(mids, found->tag)) {
                    return Error{.code = ErrorCode::DuplicateMid, .line = a.line};
                }
                mid = found;
            }
        }
        if (mid == nullptr) {
            return Error{.code = ErrorCode::MissingMid, .line = m.line};
        }
        mids.push_back(mid->tag);
    }
    return std::nullopt;
}

} // namespace

bool carries_rtp(std::string_view protocol) noexcept {
    while (true) {
        const std::size_t slash = protocol.find('/');
        if (protocol.substr(0, slash) == "RTP") {
            return true;
        }
        if (slash == std::string_view::npos) {
            return false;
        }
        protocol.remove_prefix(slash + 1);
    }
}

std::optional<Error> validate(const Session& session) {
    if (auto e = check_fingerprints(session.attributes)) {
        return e;
    }
    if (auto e = check_extmap_ids(session.attributes, {})) {
        return e;
    }
    std::vector<std::string_view> mids;
    if (auto e = collect_mids(session, mids)) {
        return e;
    }
    Bundling bundling;
    if (auto e = check_bundle(session, mids, bundling)) {
        return e;
    }
    for (std::size_t i = 0; i < session.media.size(); ++i) {
        const MediaDescription& m = session.media[i];
        if (auto e = check_fingerprints(m.attributes)) {
            return e;
        }
        if (auto e = check_extmap_ids(session.attributes, m.attributes)) {
            return e;
        }
        if (auto e = check_formats(m)) {
            return e;
        }
        if (auto e = check_simulcast(m)) {
            return e;
        }
        const bool active = m.port != 0 || is_bundle_only(m);
        const bool describes_transport = bundling.bundled[i] ? bundling.tagged[i] : active;
        if (describes_transport) {
            if (auto e = check_transport(session, m)) {
                return e;
            }
        }
    }
    return std::nullopt;
}

} // namespace codec::sdp::detail

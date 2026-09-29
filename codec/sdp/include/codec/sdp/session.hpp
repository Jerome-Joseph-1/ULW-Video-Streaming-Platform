#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

// A session description as RFC 8866 structures it, with the attributes WebRTC negotiates parsed
// into types. Every string_view points into the text given to parse(), which must outlive the
// Session; a Session built by hand points wherever its builder keeps the strings.
namespace codec::sdp {

struct Origin {
    std::string_view username;
    std::uint64_t session_id = 0;
    std::uint64_t session_version = 0;
    std::string_view network_type;
    std::string_view address_type;
    std::string_view address;

    friend bool operator==(const Origin&, const Origin&) = default;
};

struct Connection {
    std::string_view network_type;
    std::string_view address_type;
    // With a multicast TTL and address count, as written ("224.2.1.1/127/3").
    std::string_view address;

    friend bool operator==(const Connection&, const Connection&) = default;
};

struct Bandwidth {
    std::string_view type;
    // In the unit the type defines: kbit/s for AS and CT, bit/s for TIAS.
    std::uint64_t value = 0;

    friend bool operator==(const Bandwidth&, const Bandwidth&) = default;
};

struct Timing {
    std::uint64_t start = 0;
    std::uint64_t stop = 0;
    // The r= lines that follow this t=, verbatim.
    std::vector<std::string_view> repeats;

    friend bool operator==(const Timing&, const Timing&) = default;
};

enum class Direction : std::uint8_t { SendRecv, SendOnly, RecvOnly, Inactive };

// RFC 8866 section 6.6.
struct Rtpmap {
    std::uint8_t payload_type = 0;
    std::string_view encoding;
    std::uint32_t clock_rate = 0;
    std::optional<std::uint32_t> channels;

    friend bool operator==(const Rtpmap&, const Rtpmap&) = default;
};

// RFC 8866 section 6.15. The format is a token, not a payload type: fmtp is not RTP-specific.
struct Fmtp {
    std::string_view format;
    std::string_view parameters;

    friend bool operator==(const Fmtp&, const Fmtp&) = default;
};

// RFC 4585 section 4.2.
struct RtcpFeedback {
    // Empty for "*", every payload type of the section.
    std::optional<std::uint8_t> payload_type;
    std::string_view type;
    std::optional<std::string_view> parameter;

    friend bool operator==(const RtcpFeedback&, const RtcpFeedback&) = default;
};

// RFC 8285 section 8.
struct Extmap {
    std::uint16_t id = 0;
    std::optional<Direction> direction;
    std::string_view uri;
    std::optional<std::string_view> attributes;

    friend bool operator==(const Extmap&, const Extmap&) = default;
};

// RFC 5888.
struct Mid {
    std::string_view tag;

    friend bool operator==(const Mid&, const Mid&) = default;
};

struct Group {
    std::string_view semantics;
    std::vector<std::string_view> tags;

    friend bool operator==(const Group&, const Group&) = default;
};

// RFC 8830.
struct Msid {
    std::string_view stream;
    std::optional<std::string_view> track;

    friend bool operator==(const Msid&, const Msid&) = default;
};

// RFC 5576.
struct Ssrc {
    std::uint32_t ssrc = 0;
    std::string_view attribute;
    std::optional<std::string_view> value;

    friend bool operator==(const Ssrc&, const Ssrc&) = default;
};

struct SsrcGroup {
    std::string_view semantics;
    std::vector<std::uint32_t> ssrcs;

    friend bool operator==(const SsrcGroup&, const SsrcGroup&) = default;
};

// RFC 8839 section 5.
struct IceUfrag {
    std::string_view value;

    friend bool operator==(const IceUfrag&, const IceUfrag&) = default;
};

struct IcePwd {
    std::string_view value;

    friend bool operator==(const IcePwd&, const IcePwd&) = default;
};

struct IceOptions {
    std::vector<std::string_view> options;

    friend bool operator==(const IceOptions&, const IceOptions&) = default;
};

struct Candidate {
    std::string_view foundation;
    std::uint16_t component = 0;
    std::string_view transport;
    std::uint32_t priority = 0;
    std::string_view address;
    std::uint16_t port = 0;
    std::string_view type;
    std::optional<std::string_view> related_address;
    std::optional<std::uint16_t> related_port;
    // Name-value pairs after the related address and port (generation, network-cost, tcptype),
    // verbatim.
    std::optional<std::string_view> extensions;

    friend bool operator==(const Candidate&, const Candidate&) = default;
};

struct EndOfCandidates {
    friend bool operator==(EndOfCandidates, EndOfCandidates) = default;
};

// RFC 8122 section 5.
struct Fingerprint {
    std::string_view algorithm;
    // Colon-separated pairs of upper-case hex digits.
    std::string_view value;

    friend bool operator==(const Fingerprint&, const Fingerprint&) = default;
};

// RFC 4145 section 4.
enum class SetupRole : std::uint8_t { Active, Passive, ActPass, HoldConn };

struct Setup {
    SetupRole role = SetupRole::ActPass;

    friend bool operator==(const Setup&, const Setup&) = default;
};

// RFC 5761 and RFC 5506.
struct RtcpMux {
    friend bool operator==(RtcpMux, RtcpMux) = default;
};

struct RtcpRsize {
    friend bool operator==(RtcpRsize, RtcpRsize) = default;
};

// RFC 8851 and RFC 8853.
enum class RidDirection : std::uint8_t { Send, Recv };

struct Rid {
    std::string_view id;
    RidDirection direction = RidDirection::Send;
    // "pt=96,97;max-width=1280", verbatim.
    std::optional<std::string_view> restrictions;

    friend bool operator==(const Rid&, const Rid&) = default;
};

struct SimulcastStreams {
    RidDirection direction = RidDirection::Send;
    // Alternatives separated by ',' inside streams separated by ';', each rid optionally
    // paused with '~' ("q;~h,m;f"), verbatim.
    std::string_view streams;

    friend bool operator==(const SimulcastStreams&, const SimulcastStreams&) = default;
};

struct Simulcast {
    SimulcastStreams first;
    std::optional<SimulcastStreams> second;

    friend bool operator==(const Simulcast&, const Simulcast&) = default;
};

// Anything not typed above, kept as it came so that it serializes back unchanged.
struct UnknownAttribute {
    std::string_view name;
    std::optional<std::string_view> value;

    friend bool operator==(const UnknownAttribute&, const UnknownAttribute&) = default;
};

using AttributeValue =
    std::variant<UnknownAttribute, Rtpmap, Fmtp, RtcpFeedback, Extmap, Mid, Group, Msid, Ssrc,
                 SsrcGroup, IceUfrag, IcePwd, IceOptions, Candidate, EndOfCandidates, Fingerprint,
                 Setup, RtcpMux, RtcpRsize, Direction, Rid, Simulcast>;

struct Attribute {
    AttributeValue value;
    // Where the attribute was read, for error messages; 0 when it was built, not parsed.
    std::uint32_t line = 0;

    // The line is where the attribute came from, not part of what it says.
    friend bool operator==(const Attribute& a, const Attribute& b) { return a.value == b.value; }
};

struct MediaDescription {
    std::string_view media;
    std::uint16_t port = 0;
    std::optional<std::uint16_t> port_count;
    std::string_view protocol;
    std::vector<std::string_view> formats;
    std::optional<std::string_view> information;
    std::vector<Connection> connections;
    std::vector<Bandwidth> bandwidths;
    std::optional<std::string_view> key;
    std::vector<Attribute> attributes;
    std::uint32_t line = 0;

    friend bool operator==(const MediaDescription& a, const MediaDescription& b) {
        return a.media == b.media && a.port == b.port && a.port_count == b.port_count &&
               a.protocol == b.protocol && a.formats == b.formats &&
               a.information == b.information && a.connections == b.connections &&
               a.bandwidths == b.bandwidths && a.key == b.key && a.attributes == b.attributes;
    }
};

// v= is not kept: version 0 is the only one there is.
struct Session {
    Origin origin;
    std::string_view name;
    std::optional<std::string_view> information;
    std::optional<std::string_view> uri;
    std::vector<std::string_view> emails;
    std::vector<std::string_view> phones;
    std::optional<Connection> connection;
    std::vector<Bandwidth> bandwidths;
    std::vector<Timing> timings;
    std::optional<std::string_view> zone_adjustments;
    std::optional<std::string_view> key;
    std::vector<Attribute> attributes;
    std::vector<MediaDescription> media;

    friend bool operator==(const Session&, const Session&) = default;
};

template <typename T>
[[nodiscard]] const T* find_attribute(std::span<const Attribute> attributes) noexcept {
    for (const Attribute& a : attributes) {
        if (const T* value = std::get_if<T>(&a.value)) {
            return value;
        }
    }
    return nullptr;
}

} // namespace codec::sdp

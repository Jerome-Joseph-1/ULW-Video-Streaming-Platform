#include "codec/sdp/serializer.hpp"

#include "codec/sdp/session.hpp"

#include "attributes.hpp"

#include <array>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace codec::sdp {
namespace {

template <typename... F> struct Overloaded : F... {
    using F::operator()...;
};

class Writer {
public:
    explicit Writer(std::string& out) noexcept : out_(out) {}

    Writer& operator<<(std::string_view s) {
        out_ += s;
        return *this;
    }

    Writer& operator<<(char c) {
        out_ += c;
        return *this;
    }

    template <std::unsigned_integral T> Writer& operator<<(T n) {
        // 20 digits hold 2^64 - 1.
        std::array<char, 20> digits{};
        const auto [end, ec] = std::to_chars(digits.begin(), digits.end(), n);
        out_.append(digits.begin(), end);
        return *this;
    }

    Writer& line(char type) {
        out_ += type;
        out_ += '=';
        return *this;
    }

    void end() { out_ += "\r\n"; }

    template <typename T> Writer& optional(char separator, const std::optional<T>& value) {
        if (value) {
            *this << separator << *value;
        }
        return *this;
    }

private:
    std::string& out_;
};

void write_value(Writer& w, const AttributeValue& value) {
    std::visit(Overloaded{
                   [&](const UnknownAttribute& a) { w.optional(':', a.value); },
                   [&](const Rtpmap& r) {
                       w << ':' << r.payload_type << ' ' << r.encoding << '/' << r.clock_rate;
                       w.optional('/', r.channels);
                   },
                   [&](const Fmtp& f) { w << ':' << f.format << ' ' << f.parameters; },
                   [&](const RtcpFeedback& fb) {
                       w << ':';
                       if (fb.payload_type) {
                           w << *fb.payload_type;
                       } else {
                           w << '*';
                       }
                       w << ' ' << fb.type;
                       w.optional(' ', fb.parameter);
                   },
                   [&](const Extmap& e) {
                       w << ':' << e.id;
                       if (e.direction) {
                           w << '/' << detail::direction_name(*e.direction);
                       }
                       w << ' ' << e.uri;
                       w.optional(' ', e.attributes);
                   },
                   [&](const Mid& m) { w << ':' << m.tag; },
                   [&](const Group& g) {
                       w << ':' << g.semantics;
                       for (const std::string_view tag : g.tags) {
                           w << ' ' << tag;
                       }
                   },
                   [&](const Msid& m) {
                       w << ':' << m.stream;
                       w.optional(' ', m.track);
                   },
                   [&](const Ssrc& s) {
                       w << ':' << s.ssrc << ' ' << s.attribute;
                       w.optional(':', s.value);
                   },
                   [&](const SsrcGroup& g) {
                       w << ':' << g.semantics;
                       for (const std::uint32_t ssrc : g.ssrcs) {
                           w << ' ' << ssrc;
                       }
                   },
                   [&](const IceUfrag& u) { w << ':' << u.value; },
                   [&](const IcePwd& p) { w << ':' << p.value; },
                   [&](const IceOptions& o) {
                       char separator = ':';
                       for (const std::string_view option : o.options) {
                           w << separator << option;
                           separator = ' ';
                       }
                   },
                   [&](const Candidate& c) {
                       w << ':' << c.foundation << ' ' << c.component << ' ' << c.transport << ' '
                         << c.priority << ' ' << c.address << ' ' << c.port << " typ " << c.type;
                       if (c.related_address) {
                           w << " raddr " << *c.related_address;
                       }
                       if (c.related_port) {
                           w << " rport " << *c.related_port;
                       }
                       w.optional(' ', c.extensions);
                   },
                   [&](const Fingerprint& fp) { w << ':' << fp.algorithm << ' ' << fp.value; },
                   [&](const Setup& s) { w << ':' << detail::setup_name(s.role); },
                   [&](const Rid& r) {
                       w << ':' << r.id << ' ' << detail::rid_direction_name(r.direction);
                       w.optional(' ', r.restrictions);
                   },
                   [&](const Simulcast& s) {
                       w << ':' << detail::rid_direction_name(s.first.direction) << ' '
                         << s.first.streams;
                       if (s.second) {
                           w << ' ' << detail::rid_direction_name(s.second->direction) << ' '
                             << s.second->streams;
                       }
                   },
                   [](EndOfCandidates) {},
                   [](RtcpMux) {},
                   [](RtcpRsize) {},
                   [](Direction) {},
               },
               value);
}

void write_attributes(Writer& w, const std::vector<Attribute>& attributes) {
    for (const Attribute& a : attributes) {
        w.line('a') << detail::attribute_name(a.value);
        write_value(w, a.value);
        w.end();
    }
}

void write_connection(Writer& w, const Connection& c) {
    w.line('c') << c.network_type << ' ' << c.address_type << ' ' << c.address;
    w.end();
}

void write_bandwidths(Writer& w, const std::vector<Bandwidth>& bandwidths) {
    for (const Bandwidth& b : bandwidths) {
        w.line('b') << b.type << ':' << b.value;
        w.end();
    }
}

void write_text(Writer& w, char type, std::string_view text) {
    w.line(type) << text;
    w.end();
}

void write_text(Writer& w, char type, const std::optional<std::string_view>& text) {
    if (text) {
        write_text(w, type, *text);
    }
}

void write_media(Writer& w, const MediaDescription& m) {
    w.line('m') << m.media << ' ' << m.port;
    w.optional('/', m.port_count);
    w << ' ' << m.protocol;
    for (const std::string_view f : m.formats) {
        w << ' ' << f;
    }
    w.end();
    write_text(w, 'i', m.information);
    for (const Connection& c : m.connections) {
        write_connection(w, c);
    }
    write_bandwidths(w, m.bandwidths);
    write_text(w, 'k', m.key);
    write_attributes(w, m.attributes);
}

} // namespace

std::string serialize(const Session& s) {
    std::string out;
    Writer w{out};
    write_text(w, 'v', std::string_view{"0"});
    const Origin& o = s.origin;
    w.line('o') << o.username << ' ' << o.session_id << ' ' << o.session_version << ' '
                << o.network_type << ' ' << o.address_type << ' ' << o.address;
    w.end();
    write_text(w, 's', s.name);
    write_text(w, 'i', s.information);
    write_text(w, 'u', s.uri);
    for (const std::string_view e : s.emails) {
        write_text(w, 'e', e);
    }
    for (const std::string_view p : s.phones) {
        write_text(w, 'p', p);
    }
    if (s.connection) {
        write_connection(w, *s.connection);
    }
    write_bandwidths(w, s.bandwidths);
    for (const Timing& t : s.timings) {
        w.line('t') << t.start << ' ' << t.stop;
        w.end();
        for (const std::string_view r : t.repeats) {
            write_text(w, 'r', r);
        }
    }
    write_text(w, 'z', s.zone_adjustments);
    write_text(w, 'k', s.key);
    write_attributes(w, s.attributes);
    for (const MediaDescription& m : s.media) {
        write_media(w, m);
    }
    return out;
}

} // namespace codec::sdp

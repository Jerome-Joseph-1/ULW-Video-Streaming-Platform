// Every input goes to both parsers, whatever the RFC 5761 demux says of it, and whatever they
// accept must describe the datagram and nothing outside it: an RTP packet's header, CSRCs,
// extension, payload and padding tile it exactly; every extension element lies inside the
// extension; every span of every RTCP packet lies inside the datagram, and every entry and
// SDES item can be read. A reader that has failed keeps failing with the same error.
//
// Input: one datagram.

#include "codec/rtp/demux.hpp"
#include "codec/rtp/rtcp.hpp"
#include "codec/rtp/rtp.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <variant>

namespace {

void check(bool invariant) {
    if (!invariant) {
        __builtin_trap();
    }
}

template <typename... F> struct Overloaded : F... {
    using F::operator()...;
};

struct Bounds {
    std::span<const std::byte> whole;

    void operator()(std::span<const std::byte> part) const {
        if (part.empty()) {
            return;
        }
        const std::less_equal<> le;
        check(le(whole.data(), part.data()));
        check(le(&part.back(), &whole.back()));
    }
};

// Reads every entry: an out-of-range one would read past the span under the sanitizer.
template <typename T> std::uint64_t touch(const codec::rtp::Entries<T>& entries) {
    std::uint64_t n = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        static_cast<void>(entries[i]);
        ++n;
    }
    return n;
}

void check_rtp(std::span<const std::byte> datagram) {
    const auto p = codec::rtp::parse_rtp(datagram);
    if (!p) {
        return;
    }
    const Bounds inside{datagram};
    constexpr std::size_t kFixedHeader = 12;
    constexpr std::size_t kExtensionHeader = 4;
    std::size_t total = kFixedHeader + (4 * touch(p->csrcs)) + p->payload.size() + p->padding;
    if (p->extension) {
        inside(p->extension->data);
        total += kExtensionHeader + p->extension->data.size();
        const Bounds in_extension{p->extension->data};
        codec::rtp::ExtensionElements elements{*p->extension};
        while (const auto e = elements.next()) {
            check(e->id != 0);
            in_extension(e->data);
        }
        check(!elements.malformed());
    }
    inside(p->payload);
    check(total == datagram.size());
}

void check_rtcp(std::span<const std::byte> datagram) {
    const Bounds inside{datagram};
    codec::rtp::CompoundReader reader{datagram};
    while (const auto next = reader.next()) {
        if (!next->has_value()) {
            const codec::rtp::Error error = next->error();
            const auto again = reader.next();
            check(again.has_value() && !again->has_value() && again->error() == error);
            return;
        }
        std::visit(Overloaded{
                       [&](const codec::rtp::SenderReport& sr) {
                           touch(sr.reports);
                           inside(sr.extension);
                       },
                       [&](const codec::rtp::ReceiverReport& rr) {
                           touch(rr.reports);
                           inside(rr.extension);
                       },
                       [&](const codec::rtp::SourceDescription& sdes) {
                           inside(sdes.chunks);
                           codec::rtp::SdesItems items = sdes.items();
                           while (const auto item = items.next()) {
                               inside(item->value);
                           }
                           check(!items.malformed());
                       },
                       [&](const codec::rtp::Bye& bye) {
                           touch(bye.ssrcs);
                           if (bye.reason) {
                               inside(*bye.reason);
                           }
                       },
                       [&](const codec::rtp::App& app) { inside(app.data); },
                       [&](const codec::rtp::Nack& nack) { check(touch(nack.items) > 0); },
                       [&](const codec::rtp::TransportCc& cc) { inside(cc.chunks_and_deltas); },
                       [](const codec::rtp::Pli&) {},
                       [&](const codec::rtp::Fir& fir) { check(touch(fir.entries) > 0); },
                       [&](const codec::rtp::Remb& remb) { touch(remb.ssrcs); },
                       [&](const codec::rtp::Feedback& fb) { inside(fb.fci); },
                       [&](const codec::rtp::UnknownRtcp& u) { inside(u.body); },
                   },
                   **next);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> datagram = std::as_bytes(std::span{data, size});
    static_cast<void>(codec::rtp::classify(datagram));
    check_rtp(datagram);
    check_rtcp(datagram);
    return 0;
}

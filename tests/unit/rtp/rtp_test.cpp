#include "codec/rtp/error.hpp"
#include "codec/rtp/rtp.hpp"

#include "wire.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

using codec::rtp::Error;
using codec::rtp::ExtensionElement;
using codec::rtp::ExtensionElements;
using codec::rtp::HeaderExtension;
using codec::rtp::parse_rtp;
using ulw::test::bytes_of;
using ulw::test::Wire;

// V=2 with the given P, X and CC; M and PT; sequence, timestamp and SSRC.
Wire header(unsigned flags, unsigned marker_pt, std::uint16_t seq = 0x1234,
            std::uint32_t ts = 0xDEADBEEF, std::uint32_t ssrc = 0x01020304) {
    Wire w;
    w.u8(0x80U | flags).u8(marker_pt).u16(seq).u32(ts).u32(ssrc);
    return w;
}

std::vector<ExtensionElement> elements(const HeaderExtension& ext) {
    std::vector<ExtensionElement> out;
    ExtensionElements it{ext};
    while (const auto e = it.next()) {
        out.push_back(*e);
    }
    return out;
}

std::vector<std::byte> copy(std::span<const std::byte> s) {
    return {s.begin(), s.end()};
}

TEST(Rtp, ReadsTheFixedHeader) {
    const auto wire = header(0, 0x80U | 96).text("abc").take();
    const auto p = parse_rtp(wire);
    ASSERT_TRUE(p.has_value());
    EXPECT_TRUE(p->marker);
    EXPECT_EQ(p->payload_type, 96);
    EXPECT_EQ(p->sequence, 0x1234);
    EXPECT_EQ(p->timestamp, 0xDEADBEEFU);
    EXPECT_EQ(p->ssrc, 0x01020304U);
    EXPECT_TRUE(p->csrcs.empty());
    EXPECT_FALSE(p->extension.has_value());
    EXPECT_EQ(copy(p->payload), bytes_of({'a', 'b', 'c'}));
    EXPECT_EQ(p->padding, 0);
}

TEST(Rtp, MarkerBitIsNotPartOfThePayloadType) {
    const auto wire = header(0, 111).take();
    const auto p = parse_rtp(wire);
    ASSERT_TRUE(p.has_value());
    EXPECT_FALSE(p->marker);
    EXPECT_EQ(p->payload_type, 111);
    EXPECT_TRUE(p->payload.empty());
}

TEST(Rtp, ReadsContributingSources) {
    const auto wire = header(2, 96).u32(0xAAAAAAAA).u32(0x0000BBBB).text("x").take();
    const auto p = parse_rtp(wire);
    ASSERT_TRUE(p.has_value());
    ASSERT_EQ(p->csrcs.size(), 2U);
    EXPECT_EQ(p->csrcs[0], 0xAAAAAAAAU);
    EXPECT_EQ(p->csrcs[1], 0x0000BBBBU);
    EXPECT_EQ(copy(p->payload), bytes_of({'x'}));
}

TEST(Rtp, RefusesVersionsOtherThanTwo) {
    for (const unsigned first : {0x00U, 0x40U, 0xC0U}) {
        auto wire = header(0, 96).take();
        wire[0] = std::byte(first);
        EXPECT_EQ(parse_rtp(wire).error(), Error::BadVersion) << first;
    }
}

TEST(Rtp, StripsThePaddingTheLastByteCounts) {
    const auto wire = header(0x20, 96).text("abc").raw({0, 0, 0, 4}).take();
    const auto p = parse_rtp(wire);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(copy(p->payload), bytes_of({'a', 'b', 'c'}));
    EXPECT_EQ(p->padding, 4);
}

TEST(Rtp, PaddingMayTakeTheWholePayload) {
    const auto wire = header(0x20, 96).raw({0, 2}).take();
    const auto p = parse_rtp(wire);
    ASSERT_TRUE(p.has_value());
    EXPECT_TRUE(p->payload.empty());
    EXPECT_EQ(p->padding, 2);
}

TEST(Rtp, RefusesPaddingOfZeroOrLongerThanWhatFollowsTheHeader) {
    EXPECT_EQ(parse_rtp(header(0x20, 96).raw({1, 2, 0}).take()).error(), Error::BadPadding);
    EXPECT_EQ(parse_rtp(header(0x20, 96).raw({1, 2, 4}).take()).error(), Error::BadPadding);
    // The padding count may not reach back into the CSRC list.
    EXPECT_EQ(parse_rtp(header(0x21, 96).u32(7).raw({5}).take()).error(), Error::BadPadding);
    // The P bit on a packet that ends with its header: the count byte is the SSRC's last.
    EXPECT_EQ(parse_rtp(header(0x20, 96, 1, 1, 0x10).take()).error(), Error::BadPadding);
}

TEST(Rtp, EveryTruncatedHeaderIsRefused) {
    // Two CSRCs and a 2-word extension: 12 + 8 + 4 + 8 bytes of header.
    const auto whole = header(0x12, 96)
                           .u32(1)
                           .u32(2)
                           .u16(0xBEDE)
                           .u16(2)
                           .raw({0x10, 0xAA, 0x00, 0x22, 1, 2, 3, 0})
                           .text("payload")
                           .take();
    constexpr std::size_t kHeader = 32;
    ASSERT_TRUE(parse_rtp(whole).has_value());
    for (std::size_t n = 0; n < kHeader; ++n) {
        // Exactly n bytes on the heap, so a read past them trips the address sanitizer.
        const std::vector<std::byte> prefix(whole.begin(), whole.begin() + static_cast<long>(n));
        const auto p = parse_rtp(prefix);
        ASSERT_FALSE(p.has_value()) << n;
        EXPECT_EQ(p.error(), Error::Truncated) << n;
    }
}

TEST(Rtp, ReadsOneByteExtensionElementsAndSkipsPadding) {
    const auto wire = header(0x10, 96)
                          .u16(0xBEDE)
                          .u16(2)
                          .raw({0x10, 0xAA, 0x00, 0x22, 1, 2, 3, 0x00})
                          .text("p")
                          .take();
    const auto p = parse_rtp(wire);
    ASSERT_TRUE(p.has_value());
    ASSERT_TRUE(p->extension.has_value());
    EXPECT_EQ(p->extension->profile, 0xBEDE);
    const auto e = elements(*p->extension);
    ASSERT_EQ(e.size(), 2U);
    EXPECT_EQ(e[0].id, 1);
    EXPECT_EQ(copy(e[0].data), bytes_of({0xAA}));
    EXPECT_EQ(e[1].id, 2);
    EXPECT_EQ(copy(e[1].data), bytes_of({1, 2, 3}));
    EXPECT_EQ(copy(p->payload), bytes_of({'p'}));
}

TEST(Rtp, OneByteIdFifteenEndsTheExtension) {
    // After ID 15 the rest is not read, even where it would parse as an element.
    const auto wire = header(0x10, 96).u16(0xBEDE).u16(1).raw({0x10, 0xAA, 0xF0, 0x30}).take();
    const auto p = parse_rtp(wire);
    ASSERT_TRUE(p.has_value());
    const auto e = elements(*p->extension);
    ASSERT_EQ(e.size(), 1U);
    EXPECT_EQ(e[0].id, 1);
}

TEST(Rtp, OneByteIdZeroIsPaddingWhateverItsLengthNibble) {
    const auto wire = header(0x10, 96).u16(0xBEDE).u16(1).raw({0x0F, 0x10, 0xAA, 0x00}).take();
    const auto p = parse_rtp(wire);
    ASSERT_TRUE(p.has_value());
    const auto e = elements(*p->extension);
    ASSERT_EQ(e.size(), 1U);
    EXPECT_EQ(e[0].id, 1);
    EXPECT_EQ(copy(e[0].data), bytes_of({0xAA}));
}

TEST(Rtp, ReadsTwoByteExtensionElementsIncludingEmptyOnes) {
    // Profile 0x100 with application bits 0x5.
    const auto wire = header(0x10, 96)
                          .u16(0x1005)
                          .u16(2)
                          .raw({0x05, 0x00, 0x00, 0x07, 0x02, 0xAB, 0xCD, 0x00})
                          .take();
    const auto p = parse_rtp(wire);
    ASSERT_TRUE(p.has_value());
    const auto e = elements(*p->extension);
    ASSERT_EQ(e.size(), 2U);
    EXPECT_EQ(e[0].id, 5);
    EXPECT_TRUE(e[0].data.empty());
    EXPECT_EQ(e[1].id, 7);
    EXPECT_EQ(copy(e[1].data), bytes_of({0xAB, 0xCD}));
}

TEST(Rtp, RefusesAnElementThatOverrunsTheExtension) {
    // One-byte form: ID 1 with 4 bytes of data, 3 left in the word.
    EXPECT_EQ(parse_rtp(header(0x10, 96).u16(0xBEDE).u16(1).raw({0x13, 1, 2, 3}).take()).error(),
              Error::BadExtension);
    // Two-byte form: a length byte that claims 3 with 2 left.
    EXPECT_EQ(parse_rtp(header(0x10, 96).u16(0x1000).u16(1).raw({0x01, 0x03, 1, 2}).take()).error(),
              Error::BadExtension);
    // Two-byte form: an ID with no room for its length byte.
    EXPECT_EQ(parse_rtp(header(0x10, 96).u16(0x1000).u16(1).raw({0, 0, 0, 0x01}).take()).error(),
              Error::BadExtension);
}

TEST(Rtp, OtherProfilesKeepTheirExtensionOpaque) {
    const auto wire = header(0x10, 96).u16(0xABAC).u16(1).raw({0xFF, 0xFF, 0xFF, 0xFF}).take();
    const auto p = parse_rtp(wire);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->extension->profile, 0xABAC);
    EXPECT_EQ(p->extension->data.size(), 4U);
    EXPECT_TRUE(elements(*p->extension).empty());
}

TEST(Rtp, ExtensionLengthBeyondThePacketIsTruncation) {
    EXPECT_EQ(parse_rtp(header(0x10, 96).u16(0xBEDE).u16(2).raw({0x10, 1, 0, 0}).take()).error(),
              Error::Truncated);
}

TEST(Rtp, ElementsOfAHandBuiltOverrunningExtensionStopAtTheOverrun) {
    const auto data = bytes_of({0x10, 0xAA, 0x13, 1});
    ExtensionElements it{HeaderExtension{.profile = 0xBEDE, .data = data}};
    ASSERT_TRUE(it.next().has_value());
    EXPECT_FALSE(it.next().has_value());
    EXPECT_TRUE(it.malformed());
    EXPECT_FALSE(it.next().has_value());
}

class SplitMix {
public:
    explicit SplitMix(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    unsigned below(unsigned bound) noexcept { return static_cast<unsigned>(next() % bound); }

private:
    std::uint64_t state_;
};

struct Built {
    std::vector<std::byte> wire;
    std::vector<std::uint32_t> csrcs;
    std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> elements;
    std::vector<std::byte> payload;
    unsigned padding = 0;
};

// A packet with random CSRCs, an RFC 8285 extension in either form with random elements and
// padding between them, a random payload and random RTP padding.
Built random_packet(SplitMix& rng) {
    Built b;
    const unsigned cc = rng.below(16);
    const unsigned form = rng.below(3);
    b.padding = rng.below(2) == 0 ? 0 : 1 + rng.below(255);
    Wire ext;
    for (unsigned i = rng.below(6); i > 0; --i) {
        if (rng.below(4) == 0) {
            ext.u8(0);
        }
        std::vector<std::byte> data;
        if (form == 1) {
            const auto id = static_cast<std::uint8_t>(1 + rng.below(14));
            data.resize(1 + rng.below(16));
            for (auto& d : data) {
                d = std::byte(rng.below(256));
            }
            ext.u8((unsigned{id} << 4U) | static_cast<unsigned>(data.size() - 1)).append(data);
            b.elements.emplace_back(id, data);
        } else if (form == 2) {
            const auto id = static_cast<std::uint8_t>(1 + rng.below(255));
            data.resize(rng.below(40));
            for (auto& d : data) {
                d = std::byte(rng.below(256));
            }
            ext.u8(id).u8(static_cast<unsigned>(data.size())).append(data);
            b.elements.emplace_back(id, data);
        }
    }
    while (ext.size() % 4 != 0) {
        ext.u8(0);
    }
    Wire w;
    w.u8(0x80U | (b.padding != 0 ? 0x20U : 0U) | (form != 0 ? 0x10U : 0U) | cc)
        .u8(rng.below(256))
        .u16(rng.below(65536))
        .u32(static_cast<std::uint32_t>(rng.next()))
        .u32(static_cast<std::uint32_t>(rng.next()));
    for (unsigned i = 0; i < cc; ++i) {
        b.csrcs.push_back(static_cast<std::uint32_t>(rng.next()));
        w.u32(b.csrcs.back());
    }
    if (form != 0) {
        w.u16(form == 1 ? 0xBEDE : 0x1000U | rng.below(16))
            .u16(static_cast<unsigned>(ext.size() / 4))
            .append(ext.bytes());
    }
    b.payload.resize(rng.below(300));
    for (auto& d : b.payload) {
        d = std::byte(rng.below(256));
    }
    w.append(b.payload);
    for (unsigned i = 1; i < b.padding; ++i) {
        w.u8(0);
    }
    if (b.padding != 0) {
        w.u8(b.padding);
    }
    b.wire = w.take();
    return b;
}

TEST(RtpProperty, RandomPacketsParseBackToWhatWasBuilt) {
    for (std::uint64_t seed = 1; seed <= 2000; ++seed) {
        SplitMix rng{seed};
        const Built b = random_packet(rng);
        const auto p = parse_rtp(b.wire);
        ASSERT_TRUE(p.has_value()) << "seed " << seed;
        ASSERT_EQ(p->csrcs.size(), b.csrcs.size()) << "seed " << seed;
        for (std::size_t i = 0; i < b.csrcs.size(); ++i) {
            EXPECT_EQ(p->csrcs[i], b.csrcs[i]) << "seed " << seed;
        }
        EXPECT_EQ(copy(p->payload), b.payload) << "seed " << seed;
        EXPECT_EQ(p->padding, b.padding) << "seed " << seed;
        if (p->extension) {
            const auto e = elements(*p->extension);
            ASSERT_EQ(e.size(), b.elements.size()) << "seed " << seed;
            for (std::size_t i = 0; i < e.size(); ++i) {
                EXPECT_EQ(e[i].id, b.elements[i].first) << "seed " << seed;
                EXPECT_EQ(copy(e[i].data), b.elements[i].second) << "seed " << seed;
            }
        }
    }
}

} // namespace

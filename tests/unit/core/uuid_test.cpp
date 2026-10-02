#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"
#include "core/util/time.hpp"
#include "core/util/uuid.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

using core::Uuid;
using ulw::test::FakeClock;
using ulw::test::FakeRandom;

constexpr std::string_view kSample = "0192f3c4-7a1b-7c2d-8e3f-0123456789ab";

std::optional<std::string> reparse(std::string_view text) {
    return Uuid::parse(text).transform(&Uuid::to_string);
}

std::uint64_t timestamp_ms(const Uuid& id) {
    std::uint64_t ms = 0;
    for (const std::byte b : id.bytes().first<6>()) {
        ms = (ms << 8U) | std::to_integer<std::uint64_t>(b);
    }
    return ms;
}

TEST(Uuid, ParsesAndReformatsTheCanonicalForm) {
    for (const std::string_view text :
         {kSample, std::string_view("00000000-0000-0000-0000-000000000000"),
          std::string_view("ffffffff-ffff-ffff-ffff-ffffffffffff")}) {
        EXPECT_EQ(reparse(text), text);
    }
}

TEST(Uuid, DecodesDigitsInBigEndianByteOrder) {
    const Uuid id = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff").value();
    std::array<std::byte, Uuid::kByteLength> expected{};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expected.at(i) = static_cast<std::byte>(i * 0x11U);
    }
    EXPECT_TRUE(std::ranges::equal(id.bytes(), expected));
}

TEST(Uuid, RejectsEveryNonCanonicalSpelling) {
    for (const std::string_view text : {
             std::string_view(""),
             std::string_view("0192F3C4-7A1B-7C2D-8E3F-0123456789AB"),
             std::string_view("0192f3c4-7a1b-7c2d-8e3f-0123456789aB"),
             std::string_view("{0192f3c4-7a1b-7c2d-8e3f-0123456789ab}"),
             std::string_view("urn:uuid:0192f3c4-7a1b-7c2d-8e3f-0123456789ab"),
             std::string_view("0192f3c47a1b7c2d8e3f0123456789ab"),
             std::string_view("0192f3c4-7a1b-7c2d-8e3f-0123456789a"),
             std::string_view("0192f3c4-7a1b-7c2d-8e3f-0123456789abc"),
             std::string_view("0192f3c4-7a1b-7c2d-8e3f-0123456789ab\n"),
             std::string_view(" 192f3c4-7a1b-7c2d-8e3f-0123456789ab"),
             std::string_view("0192f3c-47a1b-7c2d-8e3f-0123456789ab"),
             std::string_view("0192f3c4-7a1b7-c2d-8e3f-0123456789ab"),
             std::string_view("0192f3c4-7a1b-7c2d-8e3f0-123456789ab"),
             std::string_view("0192f3c4-7a1b-7c2d-8e3f-01234567-89a"),
             std::string_view("0192f3c4_7a1b_7c2d_8e3f_0123456789ab"),
             std::string_view("0192f3c4-7a1b-7c2d-8e3g-0123456789ab"),
             std::string_view("0192f3c4-7a1b-7c2d-8e3f-0123456789a+"),
             std::string_view("0192f3c4-7a1b-7c2d-8e3f-0123456789a\0", 36),
         }) {
        EXPECT_FALSE(Uuid::parse(text).has_value()) << text;
    }
}

TEST(Uuid, FormatToWritesTheCanonicalTextIntoTheCallerBuffer) {
    std::array<char, Uuid::kTextLength> buffer{};
    Uuid::parse(kSample).value().format_to(buffer);
    EXPECT_EQ(std::string_view(buffer.data(), buffer.size()), kSample);
}

TEST(Uuid, OrdersByLeadingBytesFirst) {
    const Uuid low = Uuid::parse("0fffffff-ffff-ffff-ffff-ffffffffffff").value();
    const Uuid high = Uuid::parse("10000000-0000-0000-0000-000000000000").value();
    EXPECT_LT(low, high);
    EXPECT_NE(low, high);
}

TEST(Uuid, HashChangesWithEveryDigit) {
    const std::hash<Uuid> hash;
    const Uuid base = Uuid::parse(kSample).value();
    EXPECT_EQ(hash(Uuid::parse(kSample).value()), hash(base));
    for (std::size_t at = 0; at < kSample.size(); ++at) {
        if (kSample[at] == '-') {
            continue;
        }
        std::string text(kSample);
        text[at] = text[at] == '0' ? '1' : '0';
        EXPECT_NE(hash(Uuid::parse(text).value()), hash(base)) << "digit " << at;
    }
}

TEST(UuidV7, CarriesWallClockMillisecondsBigEndian) {
    const FakeClock clock;
    FakeRandom random;
    const Uuid id = Uuid::v7(clock, random);
    // FakeClock starts at 2026-01-01T00:00:00Z.
    EXPECT_EQ(timestamp_ms(id), 1'767'225'600'000U);
    EXPECT_TRUE(id.to_string().starts_with("019b76da-a800-7")) << id.to_string();
}

class PreEpochClock final : public core::ports::IClock {
public:
    [[nodiscard]] core::MonoTime now() const noexcept override { return {}; }
    [[nodiscard]] core::WallTime wall_now() const noexcept override {
        return core::WallTime{} - core::Millis{1};
    }
};

TEST(UuidV7, ClampsAClockBeforeTheEpochToZero) {
    const PreEpochClock clock;
    FakeRandom random;
    const Uuid id = Uuid::v7(clock, random);
    EXPECT_EQ(timestamp_ms(id), 0U);
    EXPECT_TRUE(id.to_string().starts_with("00000000-0000-7")) << id.to_string();
}

// Bytes 6-15 of a v7 id: byte 6 holds the version nibble, byte 8 the variant bits.
constexpr std::size_t kTailLength = 10;

struct TailBits {
    std::array<unsigned, kTailLength> ever_set{};
    std::array<unsigned, kTailLength> always_set{};
};

TailBits observe_tails(int draws) {
    const FakeClock clock;
    FakeRandom random;
    TailBits bits;
    bits.always_set.fill(0xFFU);
    for (int i = 0; i < draws; ++i) {
        const Uuid id = Uuid::v7(clock, random);
        const auto tail = id.bytes().subspan<6>();
        for (std::size_t k = 0; k < kTailLength; ++k) {
            bits.ever_set.at(k) |= std::to_integer<unsigned>(tail[k]);
            bits.always_set.at(k) &= std::to_integer<unsigned>(tail[k]);
        }
    }
    return bits;
}

TEST(UuidV7, FixesVersionAndVariantAndRandomisesEveryOtherBit) {
    constexpr std::array<unsigned, kTailLength> kFixedMask{0xF0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::array<unsigned, kTailLength> kFixedValue{0x70, 0, 0x80, 0, 0, 0, 0, 0, 0, 0};
    // 64 draws leave a given random bit constant with probability 2^-63.
    const TailBits bits = observe_tails(64);
    for (std::size_t k = 0; k < kTailLength; ++k) {
        const unsigned mask = kFixedMask.at(k);
        EXPECT_EQ(bits.always_set.at(k) & mask, kFixedValue.at(k)) << "byte " << 6 + k;
        EXPECT_EQ(bits.ever_set.at(k) & mask, kFixedValue.at(k)) << "byte " << 6 + k;
        EXPECT_EQ(bits.always_set.at(k) & ~mask, 0U) << "byte " << 6 + k;
        EXPECT_EQ(bits.ever_set.at(k) | mask, 0xFFU) << "byte " << 6 + k;
    }
}

TEST(UuidV7, IdsMintedInOneMillisecondDiffer) {
    const FakeClock clock;
    FakeRandom random;
    EXPECT_NE(Uuid::v7(clock, random), Uuid::v7(clock, random));
}

TEST(UuidV7, LaterIdsSortAfterEarlierOnes) {
    FakeClock clock;
    FakeRandom random;
    const Uuid first = Uuid::v7(clock, random);
    clock.advance(core::Millis{1});
    const Uuid second = Uuid::v7(clock, random);
    EXPECT_EQ(timestamp_ms(second), timestamp_ms(first) + 1);
    EXPECT_LT(first, second);
}

// Every byte `fill`, and with `bit` set as well, counting bits from the first byte handed out.
class FixedRandom final : public core::ports::IRandom {
public:
    explicit FixedRandom(std::byte fill, std::optional<std::size_t> bit = std::nullopt)
        : fill_(fill), bit_(bit) {}

    void fill(std::span<std::byte> out) noexcept override {
        for (std::byte& b : out) {
            b = fill_;
            if (bit_ && *bit_ / 8 == handed_out_) {
                b |= std::byte{1} << (*bit_ % 8);
            }
            ++handed_out_;
        }
    }

private:
    std::byte fill_;
    std::optional<std::size_t> bit_;
    std::size_t handed_out_ = 0;
};

// RFC 9562 section 5.7: the version and variant bits, and the 74 random bits around them.
constexpr std::size_t kVersionByte = 6;
constexpr std::size_t kVariantByte = 8;

unsigned tail_byte(const Uuid& id, std::size_t at) {
    return std::to_integer<unsigned>(id.bytes()[at]);
}

// The random bits of the id: bytes 6 to 15 without the version and variant bits.
unsigned random_bits_of(const Uuid& id, std::size_t at) {
    const unsigned b = tail_byte(id, at);
    if (at == kVersionByte) {
        return b & 0x0FU;
    }
    if (at == kVariantByte) {
        return b & 0x3FU;
    }
    return b;
}

void expect_version_and_variant(const Uuid& id) {
    EXPECT_EQ(tail_byte(id, kVersionByte) >> 4U, 7U);
    EXPECT_EQ(tail_byte(id, kVariantByte) >> 6U, 0b10U);
}

TEST(UuidV7, RandomBitsAllSetFillEveryRandomPositionAndNothingElse) {
    const FakeClock clock;
    FixedRandom ones(std::byte{0xFF});
    const Uuid id = Uuid::v7(clock, ones);
    expect_version_and_variant(id);
    const std::array<unsigned, 3> kRandomMask{0x0F, 0xFF, 0x3F};
    for (std::size_t at = kVersionByte; at < Uuid::kByteLength; ++at) {
        const unsigned mask = at <= kVariantByte ? kRandomMask.at(at - kVersionByte) : 0xFFU;
        EXPECT_EQ(random_bits_of(id, at), mask) << "byte " << at;
    }
}

TEST(UuidV7, RandomBitsAllClearLeaveOnlyTheVersionAndVariant) {
    const FakeClock clock;
    FixedRandom zeros(std::byte{0x00});
    const Uuid id = Uuid::v7(clock, zeros);
    expect_version_and_variant(id);
    for (std::size_t at = kVersionByte; at < Uuid::kByteLength; ++at) {
        EXPECT_EQ(random_bits_of(id, at), 0U) << "byte " << at;
    }
}

// Each random bit the source hands out lands in at most one bit of the id, no two in the same
// one, and 74 of them land: the id carries 74 independent random bits.
TEST(UuidV7, EachRandomBitReachesItsOwnPositionAndSeventyFourReachOne) {
    const FakeClock clock;
    FixedRandom zeros(std::byte{0x00});
    const Uuid base = Uuid::v7(clock, zeros);
    std::array<unsigned, Uuid::kByteLength> reached{};
    std::size_t landed = 0;
    for (std::size_t bit = 0; bit < 8 * (Uuid::kByteLength - kVersionByte); ++bit) {
        FixedRandom one(std::byte{0x00}, bit);
        const Uuid id = Uuid::v7(clock, one);
        std::size_t changed = 0;
        for (std::size_t at = 0; at < Uuid::kByteLength; ++at) {
            const unsigned diff = tail_byte(id, at) ^ tail_byte(base, at);
            changed += static_cast<std::size_t>(std::popcount(diff));
            EXPECT_EQ(reached.at(at) & diff, 0U) << "bit " << bit << " landed on another's";
            reached.at(at) |= diff;
        }
        EXPECT_LE(changed, 1U) << "bit " << bit;
        landed += changed;
    }
    EXPECT_EQ(landed, 74U);
}

TEST(UuidV7, IsReproducibleFromTheClockAndSeed) {
    const FakeClock clock;
    FakeRandom a(42);
    FakeRandom b(42);
    FakeRandom c(43);
    const Uuid id = Uuid::v7(clock, a);
    EXPECT_EQ(id, Uuid::v7(clock, b));
    EXPECT_NE(id, Uuid::v7(clock, c));
}

} // namespace

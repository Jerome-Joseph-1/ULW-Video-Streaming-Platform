// Property: however a stream is cut into reads, the decoder delivers the same frames and the
// same error. Streams and cuts come from fixed seeds, so a failure names a reproducible case.

#include "codec/ws/decoder.hpp"
#include "codec/ws/encoder.hpp"
#include "codec/ws/frame.hpp"

#include "support/fake_random.hpp"
#include "wire.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using codec::ws::ClientEncoder;
using codec::ws::CloseCode;
using codec::ws::Decoded;
using codec::ws::Frame;
using codec::ws::Opcode;
using ulw::test::Bytes;
using ulw::test::decode_in_pieces;
using ulw::test::decode_whole;
using ulw::test::message;

class SplitMix {
public:
    explicit SplitMix(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    std::size_t below(std::size_t bound) noexcept { return bound == 0 ? 0 : next() % bound; }

private:
    std::uint64_t state_;
};

// The lengths where the encoding changes, around them, and the 64-bit form.
constexpr std::array<std::size_t, 10> kLengths{0, 1, 2, 125, 126, 127, 65535, 65536, 70000, 300};

// Past the default, so that 64-bit lengths decode.
constexpr std::uint64_t kLimit = std::uint64_t{1} << 20;

// One, two, three and four byte characters, so that cuts land inside sequences.
constexpr std::array<std::string_view, 4> kChars{"a", "\xC3\xA9", "\xE2\x82\xAC",
                                                 "\xF0\x9F\x98\x80"};

Bytes random_text(SplitMix& rng, std::size_t at_least) {
    Bytes out;
    while (out.size() < at_least) {
        for (const char c : kChars.at(rng.below(kChars.size()))) {
            out.push_back(static_cast<std::byte>(c));
        }
    }
    return out;
}

Bytes random_bytes(SplitMix& rng, std::size_t n) {
    Bytes out(n);
    for (std::byte& b : out) {
        b = static_cast<std::byte>(rng.next() & 0xFFU);
    }
    return out;
}

struct Stream {
    Bytes wire;
    std::vector<Frame> expected;
};

void append(ClientEncoder& encoder, const Frame& frame, Bytes& wire) {
    ASSERT_TRUE(encoder.encode(frame, wire));
}

// Whole messages, messages in fragments with control frames between them, and control frames.
Stream random_stream(std::uint64_t seed) {
    SplitMix rng{seed};
    ulw::test::FakeRandom keys{seed};
    ClientEncoder encoder{keys};
    Stream s;
    const std::size_t count = 5 + rng.below(20);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t kind = rng.below(4);
        if (kind == 0) {
            const Frame ping = message(Opcode::Ping, random_bytes(rng, rng.below(126)));
            append(encoder, ping, s.wire);
            s.expected.push_back(ping);
            continue;
        }
        const Opcode op = rng.below(2) == 0 ? Opcode::Text : Opcode::Binary;
        const std::size_t length = kLengths.at(rng.below(kLengths.size()));
        Bytes payload = op == Opcode::Text ? random_text(rng, length) : random_bytes(rng, length);
        if (kind == 1) {
            const Frame whole = message(op, std::move(payload));
            append(encoder, whole, s.wire);
            s.expected.push_back(whole);
            continue;
        }
        const std::size_t pieces = 2 + rng.below(4);
        std::size_t offset = 0;
        for (std::size_t p = 0; p < pieces; ++p) {
            const bool last = p + 1 == pieces;
            const std::size_t n =
                last ? payload.size() - offset : rng.below(payload.size() - offset + 1);
            append(encoder,
                   {.opcode = p == 0 ? op : Opcode::Continuation,
                    .fin = last,
                    .payload = {payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                payload.begin() + static_cast<std::ptrdiff_t>(offset + n)},
                    .close_code = CloseCode::NoStatus},
                   s.wire);
            offset += n;
            if (!last && rng.below(2) == 0) {
                const Frame pong = message(Opcode::Pong, random_bytes(rng, rng.below(10)));
                append(encoder, pong, s.wire);
                s.expected.push_back(pong);
            }
        }
        s.expected.push_back(message(op, std::move(payload)));
    }
    return s;
}

void expect_same(const Decoded& actual, const std::vector<Frame>& frames,
                 std::optional<CloseCode> error) {
    EXPECT_EQ(actual.error, error);
    ASSERT_EQ(actual.frames.size(), frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(actual.frames[i], frames[i]) << "frame " << i;
    }
}

TEST(WsDecoderSplits, OneByteAtATimeDecodesAsOneWrite) {
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        SCOPED_TRACE(seed);
        const Stream s = random_stream(seed);
        constexpr std::array<std::size_t, 1> kOneByte{1};

        const Decoded whole = decode_whole(s.wire, kLimit);
        const Decoded bytewise = decode_in_pieces(s.wire, kOneByte, kLimit);

        expect_same(whole, s.expected, std::nullopt);
        expect_same(bytewise, s.expected, std::nullopt);
    }
}

TEST(WsDecoderSplits, RandomCutsDecodeAsOneWrite) {
    for (std::uint64_t seed = 100; seed < 140; ++seed) {
        SCOPED_TRACE(seed);
        const Stream s = random_stream(seed);
        SplitMix cuts{~seed};
        std::vector<std::size_t> sizes;
        for (std::size_t total = 0; total < s.wire.size();) {
            // Mostly short reads, sometimes long ones, as a socket delivers them.
            const std::size_t n = 1 + (cuts.below(4) == 0 ? cuts.below(70'000) : cuts.below(20));
            sizes.push_back(n);
            total += n;
        }

        expect_same(decode_in_pieces(s.wire, sizes, kLimit), s.expected, std::nullopt);
    }
}

TEST(WsDecoderSplits, AnErrorIsTheSameHoweverTheStreamIsCut) {
    for (std::uint64_t seed = 200; seed < 216; ++seed) {
        SCOPED_TRACE(seed);
        Stream s = random_stream(seed);
        // A text frame whose payload goes wrong at its third byte; nothing after it may count.
        const Bytes bad = ulw::test::raw_frame(0x81, ulw::test::bytes({'o', 'k', 0xF5, 'x'}));
        s.wire.insert(s.wire.end(), bad.begin(), bad.end());
        const Bytes tail = ulw::test::raw_frame(0x81, "never delivered");
        s.wire.insert(s.wire.end(), tail.begin(), tail.end());
        constexpr std::array<std::size_t, 1> kOneByte{1};
        constexpr std::array<std::size_t, 3> kUneven{7, 3, 11};

        expect_same(decode_whole(s.wire, kLimit), s.expected, CloseCode::InvalidPayload);
        expect_same(decode_in_pieces(s.wire, kOneByte, kLimit), s.expected,
                    CloseCode::InvalidPayload);
        expect_same(decode_in_pieces(s.wire, kUneven, kLimit), s.expected,
                    CloseCode::InvalidPayload);
    }
}

} // namespace

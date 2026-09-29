#include "codec/ws/decoder.hpp"
#include "codec/ws/frame.hpp"

#include "wire.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

namespace {

using codec::ws::CloseCode;
using codec::ws::Decoded;
using codec::ws::Decoder;
using codec::ws::Opcode;
using ulw::test::bytes;
using ulw::test::Bytes;
using ulw::test::close_frame;
using ulw::test::concat;
using ulw::test::decode_whole;
using ulw::test::message;
using ulw::test::raw_frame;
using ulw::test::text;

constexpr unsigned kFin = 0x80;
constexpr unsigned kText = 0x1;
constexpr unsigned kBinary = 0x2;
constexpr unsigned kClose = 0x8;
constexpr unsigned kPing = 0x9;
constexpr unsigned kPong = 0xA;

Bytes close_payload(std::uint16_t code, std::string_view reason = {}) {
    const unsigned c = code;
    return concat({bytes({c >> 8U, c & 0xFFU}), text(reason)});
}

TEST(WsDecoder, UnmasksTheMaskedHelloOfRfc6455) {
    // RFC 6455 section 5.7, "A single-frame masked text message".
    const Bytes wire = bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});

    const Decoded d = decode_whole(wire);

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 1U);
    EXPECT_EQ(d.frames[0], message(Opcode::Text, text("Hello")));
}

TEST(WsDecoder, DeliversTheMaskedPongOfRfc6455) {
    const Bytes wire = bytes({0x8a, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});

    const Decoded d = decode_whole(wire);

    ASSERT_EQ(d.frames.size(), 1U);
    EXPECT_EQ(d.frames[0], message(Opcode::Pong, text("Hello")));
}

TEST(WsDecoder, ReassemblesTheFragmentedHelloOfRfc6455) {
    const Bytes wire = concat({raw_frame(kText, "Hel"), raw_frame(kFin | 0x0, "lo")});

    const Decoded d = decode_whole(wire);

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 1U);
    EXPECT_EQ(d.frames[0], message(Opcode::Text, text("Hello")));
}

TEST(WsDecoder, ReadsSixteenAndSixtyFourBitLengths) {
    // RFC 6455 section 5.7: 256 bytes take the 16-bit form, 64 KiB the 64-bit one.
    Bytes medium(256, std::byte{0x5A});
    Bytes large(65536, std::byte{0xA5});
    const Bytes wire =
        concat({raw_frame(kFin | kBinary, medium), raw_frame(kFin | kBinary, large)});
    ASSERT_EQ(wire[1], std::byte{0x80 | 126});

    const Decoded d = decode_whole(wire);

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 2U);
    EXPECT_EQ(d.frames[0], message(Opcode::Binary, medium));
    EXPECT_EQ(d.frames[1], message(Opcode::Binary, large));
}

TEST(WsDecoder, AcceptsLengthsInALongerFormThanTheyNeed) {
    // RFC 6455 section 5.2 requires the minimal form of the sender only. "Hello" (5 bytes) in
    // the 16-bit form, then in the 64-bit form, masked with the key of section 5.7.
    const Bytes masked_hello = bytes({0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});
    const Bytes wire = concat({bytes({0x81, 0x80 | 126, 0x00, 0x05}), masked_hello,
                               bytes({0x81, 0x80 | 127, 0, 0, 0, 0, 0, 0, 0, 0x05}), masked_hello});

    const Decoded d = decode_whole(wire);

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 2U);
    EXPECT_EQ(d.frames[0], message(Opcode::Text, text("Hello")));
    EXPECT_EQ(d.frames[1], message(Opcode::Text, text("Hello")));
}

TEST(WsDecoder, DeliversControlFramesBetweenFragmentsBeforeTheMessage) {
    const Bytes wire =
        concat({raw_frame(kText, "one "), raw_frame(kFin | kPing, "p1"), raw_frame(0x0, "two "),
                raw_frame(kFin | kPong, "p2"), raw_frame(kFin | 0x0, "three")});

    const Decoded d = decode_whole(wire);

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 3U);
    EXPECT_EQ(d.frames[0], message(Opcode::Ping, text("p1")));
    EXPECT_EQ(d.frames[1], message(Opcode::Pong, text("p2")));
    EXPECT_EQ(d.frames[2], message(Opcode::Text, text("one two three")));
}

TEST(WsDecoder, DeliversEmptyFramesAndEmptyFragments) {
    const Bytes wire =
        concat({raw_frame(kFin | kText, ""), raw_frame(kFin | kPing, ""), raw_frame(kBinary, ""),
                raw_frame(0x0, "x"), raw_frame(kFin | 0x0, "")});

    const Decoded d = decode_whole(wire);

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 3U);
    EXPECT_EQ(d.frames[0], message(Opcode::Text, {}));
    EXPECT_EQ(d.frames[1], message(Opcode::Ping, {}));
    EXPECT_EQ(d.frames[2], message(Opcode::Binary, text("x")));
}

TEST(WsDecoder, RefusesAnUnmaskedClientFrame) {
    const Decoded d = decode_whole(raw_frame(kFin | kText, "Hello", /*masked=*/false));

    EXPECT_TRUE(d.frames.empty());
    EXPECT_EQ(d.error, CloseCode::ProtocolError);
}

TEST(WsDecoder, RefusesEachRsvBitWithoutAnExtension) {
    for (const unsigned rsv : {0x40U, 0x20U, 0x10U}) {
        const Decoded d = decode_whole(raw_frame(kFin | rsv | kText, "Hello"));
        EXPECT_TRUE(d.frames.empty()) << rsv;
        EXPECT_EQ(d.error, CloseCode::ProtocolError) << rsv;
    }
}

TEST(WsDecoder, RefusesEveryReservedOpcode) {
    for (const unsigned op : {0x3U, 0x4U, 0x5U, 0x6U, 0x7U, 0xBU, 0xCU, 0xDU, 0xEU, 0xFU}) {
        const Decoded d = decode_whole(raw_frame(kFin | op, ""));
        EXPECT_TRUE(d.frames.empty()) << op;
        EXPECT_EQ(d.error, CloseCode::ProtocolError) << op;
    }
}

TEST(WsDecoder, RefusesAFragmentedControlFrame) {
    const Decoded d = decode_whole(raw_frame(kPing, "p"));

    EXPECT_TRUE(d.frames.empty());
    EXPECT_EQ(d.error, CloseCode::ProtocolError);
}

TEST(WsDecoder, RefusesA126ByteCloseFrame) {
    const Bytes wire = raw_frame(kFin | kClose, close_payload(1000, std::string(124, 'r')));
    ASSERT_EQ(wire.size(), 2U + 2U + 4U + 126U);

    const Decoded d = decode_whole(wire);

    EXPECT_TRUE(d.frames.empty());
    EXPECT_EQ(d.error, CloseCode::ProtocolError);
}

TEST(WsDecoder, RefusesAnOversizedControlFrameFromItsFirstTwoBytes) {
    Decoder decoder;

    const Decoded d = decoder.feed(bytes({0x88, 0x80 | 126}));

    EXPECT_EQ(d.error, CloseCode::ProtocolError);
}

TEST(WsDecoder, AcceptsA125BytePing) {
    const Bytes payload(125, std::byte{'p'});

    const Decoded d = decode_whole(raw_frame(kFin | kPing, payload));

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 1U);
    EXPECT_EQ(d.frames[0], message(Opcode::Ping, payload));
}

TEST(WsDecoder, RefusesASixtyFourBitLengthWithTheTopBitSet) {
    Decoder decoder;

    const Decoded d = decoder.feed(
        bytes({0x82, 0x80 | 127, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 1, 2, 3, 4}));

    EXPECT_EQ(d.error, CloseCode::ProtocolError);
}

TEST(WsDecoder, RefusesAMessageOverTheLimitFromItsHeader) {
    Decoder decoder{100};
    const Bytes wire = raw_frame(kFin | kBinary, Bytes(101));

    // Header only: 2 bytes, no extended length, 4 of key.
    const Decoded d = decoder.feed(std::span{wire}.first(6));

    EXPECT_EQ(d.error, CloseCode::MessageTooBig);
}

TEST(WsDecoder, CountsEveryFragmentAgainstTheLimit) {
    const Bytes wire = concat({raw_frame(kBinary, Bytes(60)), raw_frame(kFin | 0x0, Bytes(41))});

    EXPECT_EQ(decode_whole(wire, 100).error, CloseCode::MessageTooBig);
    EXPECT_FALSE(
        decode_whole(concat({raw_frame(kBinary, Bytes(60)), raw_frame(kFin | 0x0, Bytes(40))}), 100)
            .error);
}

TEST(WsDecoder, NeverReservesPastTheLimitWhileReassembling) {
    // 7-byte fragments make the buffer grow many times; doubling unchecked would end at 1792.
    constexpr std::size_t kLimit = 1000;
    Bytes wire;
    std::size_t sent = 0;
    while (sent < kLimit) {
        const std::size_t n = std::min<std::size_t>(7, kLimit - sent);
        sent += n;
        const unsigned first = (sent == n ? kBinary : 0x0) | (sent == kLimit ? kFin : 0x0);
        const Bytes part = raw_frame(first, Bytes(n, std::byte{0x42}));
        wire.insert(wire.end(), part.begin(), part.end());
    }

    const Decoded d = decode_whole(wire, kLimit);

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 1U);
    EXPECT_EQ(d.frames[0].payload.size(), kLimit);
    EXPECT_LE(d.frames[0].payload.capacity(), kLimit);
}

TEST(WsDecoder, LeavesControlFramesOutOfTheMessageLimit) {
    const Bytes wire = concat({raw_frame(kBinary, Bytes(10)), raw_frame(kFin | kPing, Bytes(125)),
                               raw_frame(kFin | 0x0, Bytes(0))});

    const Decoded d = decode_whole(wire, 10);

    EXPECT_FALSE(d.error);
    EXPECT_EQ(d.frames.size(), 2U);
}

TEST(WsDecoder, RefusesAContinuationWithoutAMessage) {
    const Decoded d = decode_whole(raw_frame(kFin | 0x0, "lo"));

    EXPECT_EQ(d.error, CloseCode::ProtocolError);
}

TEST(WsDecoder, RefusesANewMessageBeforeTheLastOneEnded) {
    const Decoded d = decode_whole(concat({raw_frame(kText, "Hel"), raw_frame(kFin | kText, "x")}));

    EXPECT_TRUE(d.frames.empty());
    EXPECT_EQ(d.error, CloseCode::ProtocolError);
}

TEST(WsDecoder, RefusesInvalidUtf8AtTheFirstBadByte) {
    Decoder decoder;
    // 0xC0 can only start an overlong form; the frame claims 20 bytes and only 3 arrive.
    const Bytes wire = raw_frame(kFin | kText, concat({text("ok"), bytes({0xC0}), Bytes(17)}));

    const Decoded d = decoder.feed(std::span{wire}.first(6 + 3));

    EXPECT_EQ(d.error, CloseCode::InvalidPayload);
}

TEST(WsDecoder, AcceptsASequenceSplitAcrossFragments) {
    // U+20AC is E2 82 AC; it is cut after its first and after its second byte.
    const Bytes wire = concat({raw_frame(kText, bytes({0xE2})), raw_frame(0x0, bytes({0x82})),
                               raw_frame(kFin | 0x0, bytes({0xAC}))});

    const Decoded d = decode_whole(wire);

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 1U);
    EXPECT_EQ(d.frames[0], message(Opcode::Text, bytes({0xE2, 0x82, 0xAC})));
}

TEST(WsDecoder, RefusesInvalidUtf8InAFragmentBeforeTheMessageEnds) {
    // E2 opens a three-byte sequence and 0x28 cannot continue it; the final fragment never comes.
    const Bytes wire = concat({raw_frame(kText, bytes({0xE2})), raw_frame(0x0, bytes({0x28}))});

    const Decoded d = decode_whole(wire);

    EXPECT_EQ(d.error, CloseCode::InvalidPayload);
}

TEST(WsDecoder, RefusesATextMessageThatEndsInsideASequence) {
    const Decoded d = decode_whole(raw_frame(kFin | kText, bytes({'a', 0xE2, 0x82})));

    EXPECT_EQ(d.error, CloseCode::InvalidPayload);
}

TEST(WsDecoder, LeavesBinaryPayloadUnchecked) {
    const Decoded d = decode_whole(raw_frame(kFin | kBinary, bytes({0xC0, 0xFF})));

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 1U);
}

TEST(WsDecoder, DeliversACloseWithoutAStatus) {
    const Decoded d = decode_whole(raw_frame(kFin | kClose, ""));

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 1U);
    EXPECT_EQ(d.frames[0], close_frame(CloseCode::NoStatus));
}

TEST(WsDecoder, DeliversACloseStatusAndReason) {
    const Decoded d = decode_whole(raw_frame(kFin | kClose, close_payload(1001, "bye")));

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 1U);
    EXPECT_EQ(d.frames[0], close_frame(CloseCode::GoingAway, "bye"));
}

TEST(WsDecoder, RefusesAOneByteClosePayload) {
    const Decoded d = decode_whole(raw_frame(kFin | kClose, bytes({0x03})));

    EXPECT_TRUE(d.frames.empty());
    EXPECT_EQ(d.error, CloseCode::ProtocolError);
}

TEST(WsDecoder, RefusesACloseReasonThatIsNotUtf8) {
    const Decoded d = decode_whole(
        raw_frame(kFin | kClose, concat({close_payload(1000), bytes({0xED, 0xA0, 0x80})})));

    EXPECT_TRUE(d.frames.empty());
    EXPECT_EQ(d.error, CloseCode::InvalidPayload);
}

class WsCloseCodeOnTheWire : public ::testing::TestWithParam<std::uint16_t> {};

TEST_P(WsCloseCodeOnTheWire, IsRefusedAsAProtocolError) {
    const Decoded d = decode_whole(raw_frame(kFin | kClose, close_payload(GetParam())));

    EXPECT_TRUE(d.frames.empty());
    EXPECT_EQ(d.error, CloseCode::ProtocolError);
}

INSTANTIATE_TEST_SUITE_P(ReservedAndUnassigned, WsCloseCodeOnTheWire,
                         ::testing::Values(0, 999, 1004, 1005, 1006, 1015, 1016, 1100, 2000, 2999,
                                           5000, 65535));

class WsValidCloseCode : public ::testing::TestWithParam<std::uint16_t> {};

TEST_P(WsValidCloseCode, IsDeliveredAsSent) {
    const Decoded d = decode_whole(raw_frame(kFin | kClose, close_payload(GetParam())));

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 1U);
    EXPECT_EQ(d.frames[0].close_code.value, GetParam());
}

INSTANTIATE_TEST_SUITE_P(RegisteredAndPrivate, WsValidCloseCode,
                         ::testing::Values(1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011,
                                           1012, 1013, 1014, 3000, 3999, 4000, 4999));

TEST(WsDecoder, IgnoresEverythingAfterAClose) {
    Decoder decoder;
    const Decoded first = decoder.feed(
        concat({raw_frame(kFin | kClose, close_payload(1000)), raw_frame(kFin | kText, "late")}));
    const Decoded later = decoder.feed(bytes({0xFF, 0xFF}));

    ASSERT_EQ(first.frames.size(), 1U);
    EXPECT_EQ(first.frames[0].opcode, Opcode::Close);
    EXPECT_FALSE(first.error);
    EXPECT_TRUE(decoder.closed());
    EXPECT_TRUE(later.frames.empty());
    EXPECT_FALSE(later.error);
}

TEST(WsDecoder, DeliversWhatCameBeforeAnError) {
    const Decoded d =
        decode_whole(concat({raw_frame(kFin | kText, "first"), raw_frame(kFin | kPing, "p"),
                             raw_frame(kFin | 0x40 | kText, "bad"), raw_frame(kFin | kText, "x")}));

    ASSERT_EQ(d.frames.size(), 2U);
    EXPECT_EQ(d.frames[0], message(Opcode::Text, text("first")));
    EXPECT_EQ(d.frames[1], message(Opcode::Ping, text("p")));
    EXPECT_EQ(d.error, CloseCode::ProtocolError);
}

TEST(WsDecoder, StaysFailedAndRepeatsItsStatus) {
    Decoder decoder;
    ASSERT_EQ(decoder.feed(raw_frame(kFin | kText, bytes({0xFF}))).error,
              CloseCode::InvalidPayload);

    const Decoded again = decoder.feed(raw_frame(kFin | kText, "valid"));

    EXPECT_TRUE(again.frames.empty());
    EXPECT_EQ(again.error, CloseCode::InvalidPayload);
    EXPECT_EQ(decoder.error(), CloseCode::InvalidPayload);
    EXPECT_EQ(decoder.feed({}).error, CloseCode::InvalidPayload);
}

TEST(WsDecoder, DecodesTheSameMessageAgainAfterDeliveringOne) {
    // The reassembly buffer is handed over with the message; the next one starts empty.
    Decoder decoder;
    const Bytes frame = concat({raw_frame(kBinary, "ab"), raw_frame(kFin | 0x0, "cd")});

    const Decoded first = decoder.feed(frame);
    const Decoded second = decoder.feed(frame);

    ASSERT_EQ(first.frames.size(), 1U);
    ASSERT_EQ(second.frames.size(), 1U);
    EXPECT_EQ(first.frames[0], message(Opcode::Binary, text("abcd")));
    EXPECT_EQ(second.frames[0], first.frames[0]);
}

} // namespace

#include "codec/ws/decoder.hpp"
#include "codec/ws/encoder.hpp"
#include "codec/ws/frame.hpp"

#include "wire.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <gtest/gtest.h>
#include <string>

namespace {

using codec::ws::ClientEncoder;
using codec::ws::CloseCode;
using codec::ws::encode;
using codec::ws::EncodeError;
using codec::ws::Frame;
using codec::ws::Opcode;
using ulw::test::bytes;
using ulw::test::Bytes;
using ulw::test::close_frame;
using ulw::test::FixedKeys;
using ulw::test::message;
using ulw::test::text;

Bytes encoded(const Frame& frame) {
    Bytes out;
    EXPECT_TRUE(encode(frame, out));
    return out;
}

TEST(WsEncoder, WritesTheUnmaskedHelloOfRfc6455) {
    EXPECT_EQ(encoded(message(Opcode::Text, text("Hello"))),
              bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f}));
}

TEST(WsEncoder, WritesTheFragmentedHelloOfRfc6455) {
    Bytes out;
    ASSERT_TRUE(encode({.opcode = Opcode::Text,
                        .fin = false,
                        .payload = text("Hel"),
                        .close_code = CloseCode::NoStatus},
                       out));
    ASSERT_TRUE(encode({.opcode = Opcode::Continuation,
                        .fin = true,
                        .payload = text("lo"),
                        .close_code = CloseCode::NoStatus},
                       out));

    EXPECT_EQ(out, bytes({0x01, 0x03, 0x48, 0x65, 0x6c, 0x80, 0x02, 0x6c, 0x6f}));
}

TEST(WsEncoder, WritesTheUnmaskedPingOfRfc6455) {
    EXPECT_EQ(encoded(message(Opcode::Ping, text("Hello"))),
              bytes({0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f}));
}

TEST(WsEncoder, WritesTheLengthFormsOfRfc6455) {
    const Bytes medium = encoded(message(Opcode::Binary, Bytes(256)));
    const Bytes large = encoded(message(Opcode::Binary, Bytes(65536)));

    EXPECT_EQ(Bytes(medium.begin(), medium.begin() + 4), bytes({0x82, 0x7E, 0x01, 0x00}));
    EXPECT_EQ(medium.size(), 4U + 256U);
    EXPECT_EQ(Bytes(large.begin(), large.begin() + 10),
              bytes({0x82, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}));
    EXPECT_EQ(large.size(), 10U + 65536U);
}

TEST(WsEncoder, UsesTheShortestLengthAtEachBoundary) {
    EXPECT_EQ(encoded(message(Opcode::Binary, Bytes(125)))[1], std::byte{125});
    EXPECT_EQ(encoded(message(Opcode::Binary, Bytes(126)))[1], std::byte{126});
    EXPECT_EQ(encoded(message(Opcode::Binary, Bytes(65535))).size(), 4U + 65535U);
    EXPECT_EQ(encoded(message(Opcode::Binary, Bytes(65536)))[1], std::byte{127});
}

TEST(WsEncoder, WritesACloseStatusBeforeItsReason) {
    EXPECT_EQ(encoded(close_frame(CloseCode::ProtocolError, "no")),
              bytes({0x88, 0x04, 0x03, 0xEA, 'n', 'o'}));
    EXPECT_EQ(encoded(close_frame(CloseCode::NoStatus)), bytes({0x88, 0x00}));
}

TEST(WsEncoder, RefusesAStatusThatNeverGoesOnTheWire) {
    Bytes out;
    EXPECT_EQ(encode(close_frame(CloseCode{1006}), out),
              std::unexpected(EncodeError::InvalidClose));
    EXPECT_EQ(encode(close_frame(CloseCode::NoStatus, "reason"), out),
              std::unexpected(EncodeError::InvalidClose));
    EXPECT_TRUE(out.empty());
}

TEST(WsEncoder, RefusesAControlFrameOver125Bytes) {
    Bytes out;
    EXPECT_EQ(encode(message(Opcode::Ping, Bytes(126)), out),
              std::unexpected(EncodeError::ControlTooLong));
    // 2 status bytes and a 124-byte reason make 126.
    EXPECT_EQ(encode(close_frame(CloseCode::Normal, std::string(124, 'r')), out),
              std::unexpected(EncodeError::ControlTooLong));
    EXPECT_TRUE(encode(close_frame(CloseCode::Normal, std::string(123, 'r')), out));
}

TEST(WsEncoder, RefusesAFragmentedControlFrame) {
    Bytes out;
    EXPECT_EQ(encode({.opcode = Opcode::Pong,
                      .fin = false,
                      .payload = {},
                      .close_code = CloseCode::NoStatus},
                     out),
              std::unexpected(EncodeError::ControlFragmented));
}

TEST(WsEncoder, WritesAHeldPayloadAsTheSameFrame) {
    for (const std::size_t size : {std::size_t{0}, std::size_t{125}, std::size_t{126},
                                   std::size_t{65535}, std::size_t{65536}}) {
        Bytes payload(size);
        for (std::size_t i = 0; i < size; ++i) {
            payload[i] = static_cast<std::byte>(i * 7);
        }
        Bytes out;
        ASSERT_TRUE(encode(Opcode::Text, true, payload, out)) << size;
        EXPECT_EQ(out, encoded(message(Opcode::Text, payload))) << size;
    }
    Bytes out;
    ASSERT_TRUE(encode(Opcode::Text, false, text("Hel"), out));
    ASSERT_TRUE(encode(Opcode::Continuation, true, text("lo"), out));
    EXPECT_EQ(out, bytes({0x01, 0x03, 0x48, 0x65, 0x6c, 0x80, 0x02, 0x6c, 0x6f}));
}

TEST(WsEncoder, RefusesAHeldPayloadThatNoFrameCouldCarry) {
    Bytes out;
    EXPECT_EQ(encode(Opcode::Ping, true, Bytes(126), out),
              std::unexpected(EncodeError::ControlTooLong));
    EXPECT_EQ(encode(Opcode::Pong, false, Bytes{}, out),
              std::unexpected(EncodeError::ControlFragmented));
    // A reason needs a status, which this form has none of.
    EXPECT_EQ(encode(Opcode::Close, true, text("bye"), out),
              std::unexpected(EncodeError::InvalidClose));
    EXPECT_TRUE(out.empty());
    ASSERT_TRUE(encode(Opcode::Close, true, Bytes{}, out));
    EXPECT_EQ(out, bytes({0x88, 0x00}));
}

TEST(WsEncoder, AppendsFrameAfterFrameToOneBuffer) {
    Bytes out;
    Bytes expected;
    for (std::size_t i = 0; i < 64; ++i) {
        const Bytes payload(i * 100, std::byte{0x2a});
        ASSERT_TRUE(encode(Opcode::Binary, true, payload, out));
        const Bytes one = encoded(message(Opcode::Binary, payload));
        expected.insert(expected.end(), one.begin(), one.end());
    }
    EXPECT_EQ(out, expected);
}

TEST(WsClientEncoder, WritesTheMaskedHelloOfRfc6455) {
    FixedKeys keys{bytes({0x37, 0xfa, 0x21, 0x3d})};
    ClientEncoder encoder{keys};
    Bytes out;

    ASSERT_TRUE(encoder.encode(message(Opcode::Text, text("Hello")), out));

    EXPECT_EQ(out, bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58}));
}

TEST(WsClientEncoder, TakesAFreshKeyForEveryFrame) {
    FixedKeys keys{bytes({1, 2, 3, 4, 5, 6, 7, 8})};
    ClientEncoder encoder{keys};
    Bytes out;

    ASSERT_TRUE(encoder.encode(message(Opcode::Binary, {}), out));
    ASSERT_TRUE(encoder.encode(message(Opcode::Binary, {}), out));

    EXPECT_EQ(out, bytes({0x82, 0x80, 1, 2, 3, 4, 0x82, 0x80, 5, 6, 7, 8}));
}

TEST(WsClientEncoder, RoundTripsThroughTheDecoder) {
    FixedKeys keys{bytes({0xDE, 0xAD, 0xBE, 0xEF})};
    ClientEncoder encoder{keys};
    const Frame close = close_frame(CloseCode{4000}, "done");
    const Frame binary = message(Opcode::Binary, ulw::test::text(std::string(70'000, 'z')));
    Bytes wire;
    ASSERT_TRUE(encoder.encode(binary, wire));
    ASSERT_TRUE(encoder.encode(close, wire));

    codec::ws::Decoder decoder{100'000};
    const codec::ws::Decoded d = decoder.feed(wire);

    EXPECT_FALSE(d.error);
    ASSERT_EQ(d.frames.size(), 2U);
    EXPECT_EQ(d.frames[0], binary);
    EXPECT_EQ(d.frames[1], close);
}

} // namespace

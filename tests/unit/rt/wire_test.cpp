#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "wire.hpp"

#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace wire = rt::wire;

std::vector<std::byte> bytes(std::string_view text) {
    const auto b = std::as_bytes(std::span{text});
    return {b.begin(), b.end()};
}

std::string text(std::span<const std::byte> b) {
    std::string out;
    for (const std::byte x : b) {
        out += static_cast<char>(x);
    }
    return out;
}

// A frame with the given type byte and fields, however wrong they are.
std::vector<std::byte> raw(std::uint8_t type, std::string_view fields) {
    std::vector<std::byte> out;
    const auto length = static_cast<std::uint32_t>(fields.size() + 1);
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((length >> static_cast<unsigned>(shift)) & 0xFFU));
    }
    out.push_back(static_cast<std::byte>(type));
    const auto b = bytes(fields);
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

class WireTest : public ::testing::Test {
protected:
    std::vector<wire::Frame> decode_all(std::span<const std::byte> in) {
        decoder_.feed(in);
        std::vector<wire::Frame> out;
        while (true) {
            auto next = decoder_.next();
            EXPECT_TRUE(next) << "decode error";
            if (!next || !next->has_value()) {
                return out;
            }
            out.push_back(next->value());
        }
    }

    std::optional<wire::DecodeError> error_of(std::span<const std::byte> in) {
        decoder_.feed(in);
        while (true) {
            auto next = decoder_.next();
            if (!next) {
                return next.error();
            }
            if (!*next) {
                return std::nullopt;
            }
        }
    }

    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
    const core::RoomId room_ = core::RoomId::generate(clock_, random_);
    const core::UserId alice_ = *core::UserId::parse("auth0|alice");
    const rt::MessageKey key_ = *rt::MessageKey::parse("01J9ZQ4V7B8K3M2N5P6R7S8T9W");
    const core::NodeId node_ = *core::NodeId::parse("chat-7f9c-x2");
    wire::Decoder decoder_;
};

TEST_F(WireTest, EveryFrameComesBackAsItWasSent) {
    const auto body = bytes("opaque \x01\x02 bytes");
    std::vector<std::byte> out;
    wire::Nonce nonce{};
    nonce.fill(std::byte{0x42});
    wire::Mac mac{};
    mac.fill(std::byte{0x17});
    wire::encode_hello(out, node_, nonce);
    wire::encode_challenge(out, node_, nonce, mac);
    wire::encode_proof(out, mac);
    wire::encode_subscribe(out, 7, room_);
    wire::encode_unsubscribe(out, room_);
    wire::encode_send(out, 1ULL << 40U, room_, alice_, key_, body);
    wire::encode_reply(out, 9, wire::Status::Fenced, 12);
    wire::encode_deliver(out, room_, 13, alice_, key_, body);

    auto frames = decode_all(out);
    ASSERT_EQ(frames.size(), 8U);
    const auto& hello = std::get<wire::Hello>(frames[0]);
    EXPECT_EQ(hello.version, wire::kVersion);
    EXPECT_EQ(hello.node, node_);
    EXPECT_EQ(hello.nonce, nonce);
    const auto& challenge = std::get<wire::Challenge>(frames[1]);
    EXPECT_EQ(challenge.node, node_);
    EXPECT_EQ(challenge.nonce, nonce);
    EXPECT_EQ(challenge.mac, mac);
    EXPECT_EQ(std::get<wire::Proof>(frames[2]).mac, mac);
    // The rest keep their places once the handshake frames are set aside.
    frames.erase(frames.begin() + 1, frames.begin() + 3);
    const auto& subscribe = std::get<wire::Subscribe>(frames[1]);
    EXPECT_EQ(subscribe.request, 7U);
    EXPECT_EQ(subscribe.room, room_);
    EXPECT_EQ(std::get<wire::Unsubscribe>(frames[2]).room, room_);
    const auto& send = std::get<wire::Send>(frames[3]);
    EXPECT_EQ(send.request, 1ULL << 40U);
    EXPECT_EQ(send.room, room_);
    EXPECT_EQ(send.sender, alice_);
    EXPECT_EQ(send.key, key_);
    EXPECT_EQ(text(send.body), text(body));
    const auto& reply = std::get<wire::Reply>(frames[4]);
    EXPECT_EQ(reply.request, 9U);
    EXPECT_EQ(reply.status, wire::Status::Fenced);
    EXPECT_EQ(reply.seq, 12U);
    const auto& deliver = std::get<wire::Deliver>(frames[5]);
    EXPECT_EQ(deliver.room, room_);
    EXPECT_EQ(deliver.seq, 13U);
    EXPECT_EQ(deliver.sender, alice_);
    EXPECT_EQ(deliver.key, key_);
    EXPECT_EQ(text(deliver.body), text(body));
}

TEST_F(WireTest, AnAskAndItsAnswerComeBackAsTheyWereSent) {
    const auto body = bytes("ask \x00 me");
    std::vector<std::byte> out;
    wire::encode_ask(out, 21, room_, body);
    wire::encode_answer(out, 21, wire::Status::Ok, bytes("answer"));
    wire::encode_answer(out, 22, wire::Status::NotOwner, {});
    const auto frames = decode_all(out);
    ASSERT_EQ(frames.size(), 3U);
    const auto& ask = std::get<wire::Ask>(frames[0]);
    EXPECT_EQ(ask.request, 21U);
    EXPECT_EQ(ask.room, room_);
    EXPECT_EQ(text(ask.body), text(body));
    const auto& answer = std::get<wire::Answer>(frames[1]);
    EXPECT_EQ(answer.request, 21U);
    EXPECT_EQ(answer.status, wire::Status::Ok);
    EXPECT_EQ(text(answer.body), "answer");
    const auto& refused = std::get<wire::Answer>(frames[2]);
    EXPECT_EQ(refused.status, wire::Status::NotOwner);
    EXPECT_TRUE(refused.body.empty());
}

TEST_F(WireTest, AnAnswerWithAStatusNobodyDefinedIsMalformed) {
    std::string answer(8, '\0');
    answer += '\x06';
    EXPECT_EQ(error_of(raw(10, answer)), wire::DecodeError::Malformed);
}

TEST_F(WireTest, AnEmptyBodyAndTheLargestBodyBothTravel) {
    std::vector<std::byte> out;
    wire::encode_send(out, 1, room_, alice_, key_, {});
    const std::vector<std::byte> largest(wire::kMaxBody, std::byte{0x5A});
    wire::encode_deliver(out, room_, 2, alice_, key_, largest);
    const auto frames = decode_all(out);
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_TRUE(std::get<wire::Send>(frames[0]).body.empty());
    EXPECT_EQ(std::get<wire::Deliver>(frames[1]).body.size(), wire::kMaxBody);
}

TEST_F(WireTest, BytesFedOneAtATimeDecodeToTheSameFrames) {
    std::vector<std::byte> out;
    wire::encode_subscribe(out, 3, room_);
    wire::encode_deliver(out, room_, 4, alice_, key_, bytes("hi"));
    std::vector<wire::Frame> frames;
    for (const std::byte b : out) {
        const auto got = decode_all(std::span{&b, 1});
        frames.insert(frames.end(), got.begin(), got.end());
        // A view into the decoder's buffer dies at the next feed; check it before then.
        if (!got.empty()) {
            if (const auto* deliver = std::get_if<wire::Deliver>(&got.back())) {
                EXPECT_EQ(text(deliver->body), "hi");
            }
        }
    }
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(std::get<wire::Subscribe>(frames[0]).request, 3U);
    EXPECT_EQ(std::get<wire::Deliver>(frames[1]).seq, 4U);
}

TEST_F(WireTest, ALengthPastTheLimitIsRefusedBeforeItsPayloadArrives) {
    const auto too_long = static_cast<std::uint32_t>(wire::kMaxFrame + 1);
    std::vector<std::byte> header;
    for (int shift = 24; shift >= 0; shift -= 8) {
        header.push_back(
            static_cast<std::byte>((too_long >> static_cast<unsigned>(shift)) & 0xFFU));
    }
    EXPECT_EQ(error_of(header), wire::DecodeError::BadLength);
}

TEST_F(WireTest, AZeroLengthIsRefused) {
    EXPECT_EQ(error_of(std::vector<std::byte>(4, std::byte{0})), wire::DecodeError::BadLength);
}

TEST_F(WireTest, AnUnknownTypeIsRefused) {
    EXPECT_EQ(error_of(raw(0, "")), wire::DecodeError::UnknownType);
    wire::Decoder fresh;
    fresh.feed(raw(11, ""));
    EXPECT_EQ(fresh.next(), std::unexpected(wire::DecodeError::UnknownType));
}

TEST_F(WireTest, MissingOrLeftoverFieldsAreMalformed) {
    std::vector<std::byte> good;
    wire::encode_subscribe(good, 1, room_);
    auto leftover = good;
    leftover.push_back(std::byte{0});
    leftover[3] = static_cast<std::byte>(std::to_integer<unsigned>(leftover[3]) + 1);
    EXPECT_EQ(error_of(leftover), wire::DecodeError::Malformed);

    wire::Decoder fresh;
    auto shortened = good;
    shortened.pop_back();
    shortened[3] = static_cast<std::byte>(std::to_integer<unsigned>(shortened[3]) - 1);
    fresh.feed(shortened);
    EXPECT_EQ(fresh.next(), std::unexpected(wire::DecodeError::Malformed));
}

TEST_F(WireTest, ARoomOrSenderThatDoesNotParseIsMalformed) {
    // An uppercase uuid is not the canonical form.
    std::string room = room_.to_string();
    room[0] = 'A';
    EXPECT_EQ(error_of(raw(3, room)), wire::DecodeError::Malformed);

    wire::Decoder fresh;
    std::string send(8, '\0');
    send += room_.to_string();
    send += '\x03';
    send += "a b";
    fresh.feed(raw(4, send));
    EXPECT_EQ(fresh.next(), std::unexpected(wire::DecodeError::Malformed));
}

TEST_F(WireTest, ASendWhoseKeyDoesNotParseIsMalformed) {
    const auto send_with = [&](std::string_view key) {
        std::string send(8, '\0');
        send += room_.to_string();
        send += '\x05';
        send += "alice";
        send += static_cast<char>(key.size());
        send += key;
        send += "body";
        wire::Decoder fresh;
        fresh.feed(raw(4, send));
        return fresh.next();
    };
    const auto good = send_with("k-1_Z");
    ASSERT_TRUE(good && *good);
    EXPECT_EQ(std::get<wire::Send>(**good).key.view(), "k-1_Z");
    const std::string too_long(rt::MessageKey::kMaxLength + 1, 'k');
    for (const std::string_view key :
         {std::string_view{}, std::string_view{"a b"}, std::string_view{"caf\xc3\xa9"},
          std::string_view{too_long}}) {
        EXPECT_EQ(send_with(key), std::unexpected(wire::DecodeError::Malformed)) << key;
    }
}

TEST_F(WireTest, ASendOrDeliverWithABodyPastTheLargestIsMalformed) {
    // The frame bound leaves room for the other fields; with a one-letter sender and key, a
    // body can take some of it, which the frame length alone would let through.
    const auto frame = [&](std::uint8_t type, std::size_t body) {
        std::string fields;
        if (type == 4) {
            fields += std::string(8, '\0');
            fields += room_.to_string();
        } else {
            fields += room_.to_string();
            fields += std::string(8, '\0');
            fields[fields.size() - 1] = '\x01';
        }
        fields += "\x01"
                  "a"
                  "\x01"
                  "k";
        fields += std::string(body, 'x');
        return raw(type, fields);
    };
    for (const std::uint8_t type : {std::uint8_t{4}, std::uint8_t{6}}) {
        wire::Decoder largest;
        largest.feed(frame(type, wire::kMaxBody));
        const auto fits = largest.next();
        ASSERT_TRUE(fits && *fits) << int{type};

        wire::Decoder over;
        over.feed(frame(type, wire::kMaxBody + 1));
        EXPECT_EQ(over.next(), std::unexpected(wire::DecodeError::Malformed)) << int{type};
    }
}

TEST_F(WireTest, AReplyWithAStatusNobodyDefinedIsMalformed) {
    std::string reply(8, '\0');
    reply += '\x06';
    reply += std::string(8, '\0');
    EXPECT_EQ(error_of(raw(5, reply)), wire::DecodeError::Malformed);
}

TEST_F(WireTest, NothingIsDecodedAfterAnError) {
    std::vector<std::byte> in = raw(0, "");
    wire::encode_subscribe(in, 1, room_);
    EXPECT_EQ(error_of(in), wire::DecodeError::UnknownType);
    EXPECT_FALSE(decoder_.next());
}

} // namespace

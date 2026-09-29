#include "core/util/json.hpp"

#include "envelope.hpp"

#include <gtest/gtest.h>
#include <string>
#include <variant>

namespace {

using chat::EnvelopeError;

constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";

core::RoomId room() {
    return *core::RoomId::parse(kRoom);
}

TEST(Envelope, AJoinNamesItsRoom) {
    const auto c =
        chat::parse_command(R"({"type":"join","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd"})");
    ASSERT_TRUE(c);
    EXPECT_EQ(std::get<chat::Join>(*c).room, room());
}

TEST(Envelope, ASendCarriesItsBodyAsItCameAndAnOptionalRef) {
    const auto c = chat::parse_command(
        R"({"ref":42,"type":"send","body":"café \"quoted\"\n","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd"})");
    ASSERT_TRUE(c);
    const auto& send = std::get<chat::Send>(*c);
    EXPECT_EQ(send.room, room());
    EXPECT_EQ(send.ref, 42U);
    EXPECT_EQ(send.body, "caf\xc3\xa9 \"quoted\"\n");

    const auto bare = chat::parse_command(
        R"({"type":"send","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","body":""})");
    ASSERT_TRUE(bare);
    EXPECT_FALSE(std::get<chat::Send>(*bare).ref);
}

TEST(Envelope, WhatIsNotACommandIsRefusedWithAReason) {
    EXPECT_EQ(chat::parse_command("{"), std::unexpected(EnvelopeError::NotJson));
    EXPECT_EQ(chat::parse_command("[]"), std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(
        chat::parse_command(R"({"type":"leave","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd"})"),
        std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd"})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"join"})"), std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(
        chat::parse_command(R"({"type":"join","room":"01A0EB86-6CCA-7DCE-84CC-3BB47615F9FD"})"),
        std::unexpected(EnvelopeError::BadRoom));
    EXPECT_EQ(
        chat::parse_command(R"({"type":"send","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd"})"),
        std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(
                  R"({"type":"send","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","body":7})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(
        chat::parse_command(
            R"({"type":"send","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","body":"","ref":-1})"),
        std::unexpected(EnvelopeError::Malformed));
    // A misspelt field is refused, not ignored.
    EXPECT_EQ(
        chat::parse_command(
            R"({"type":"send","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","body":"","reff":1})"),
        std::unexpected(EnvelopeError::Malformed));
    // A second copy of a field is refused by the parser.
    EXPECT_EQ(chat::parse_command(
                  R"({"type":"join","type":"send","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd"})"),
              std::unexpected(EnvelopeError::NotJson));
}

TEST(Envelope, AMessageReturnsItsBodyExactlyAndEscapedAsJson) {
    const std::string body = "line one\n\"two\"\\ \x7f caf\xc3\xa9";
    const auto sender = *core::UserId::parse("auth0|alice");
    std::string out;
    chat::write_message(
        out, {.room = room(), .seq = 9, .sender = sender, .body = std::as_bytes(std::span{body})});
    const auto json = core::json::parse(out);
    ASSERT_TRUE(json) << out;
    EXPECT_EQ(json->find("type")->as_string(), "message");
    EXPECT_EQ(json->find("room")->as_string(), kRoom);
    EXPECT_EQ(json->find("seq")->as_u64(), 9U);
    EXPECT_EQ(json->find("sender")->as_string(), "auth0|alice");
    EXPECT_EQ(json->find("body")->as_string(), body);
}

TEST(Envelope, RepliesAreTheDocumentedShapes) {
    std::string out;
    chat::write_joined(out, room());
    EXPECT_EQ(out, R"({"type":"joined","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd"})");
    out.clear();
    chat::write_sent(out, room(), 3, 17);
    EXPECT_EQ(out,
              R"({"type":"sent","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","ref":3,"seq":17})");
    out.clear();
    chat::write_sent(out, room(), std::nullopt, 17);
    EXPECT_EQ(out, R"({"type":"sent","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","seq":17})");
    out.clear();
    chat::write_error(out, chat::reason(rt::RouteError::Fenced), room(), 3);
    EXPECT_EQ(
        out,
        R"({"type":"error","reason":"fenced","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","ref":3})");
    out.clear();
    chat::write_error(out, chat::reason(EnvelopeError::NotJson));
    EXPECT_EQ(out, R"({"type":"error","reason":"not_json"})");
}

} // namespace

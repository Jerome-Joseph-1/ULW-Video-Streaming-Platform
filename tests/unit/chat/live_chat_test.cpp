#include "core/ports/message_store.hpp"

#include "live_chat.hpp"

#include <format>
#include <gtest/gtest.h>
#include <string>

namespace {

using core::ports::is_stream_chat;

TEST(LiveChatRoom, IsTheDocumentedDigestOfTheStreamName) {
    // The stream-chat tag 0x01, then SHA-256("ulw-live-chat:show-1")'s first 15 bytes, version 8
    // and the RFC 9562 variant: what
    // docs/integration/chat.md tells a client to compute, and live_chat_room() in SQL gives.
    const auto room = chat::live_chat_room("show-1");
    ASSERT_TRUE(room);
    EXPECT_EQ(room->to_string(), "011b9ed0-d6b6-88e6-ac34-32d7070ba83b");
}

TEST(LiveChatRoom, EveryStreamHasItsOwnRoomAndOnlyStreamRoomsAreLive) {
    const core::RoomId show = *chat::live_chat_room("show-1");
    EXPECT_EQ(chat::live_chat_room("show-1"), show);
    EXPECT_NE(chat::live_chat_room("show-2"), show);
    EXPECT_NE(chat::live_chat_room("Show-1"), show);
    EXPECT_TRUE(is_stream_chat(show));
    EXPECT_TRUE(is_stream_chat(*chat::live_chat_room(std::string(64, 'z'))));
    // Room ids minted here are version 7, and so is any other a client has been given.
    EXPECT_FALSE(is_stream_chat(*core::RoomId::parse("01a0eb86-6cca-7dce-84cc-3bb47615f9fd")));
    EXPECT_FALSE(is_stream_chat(*core::RoomId::parse("011b9ed0-d6b6-48e6-ac34-32d7070ba83b")));
}

TEST(LiveChatRoom, AVersion8IdOfAnyOtherTagIsNotAStreamsChat) {
    // The stream's own id under every other first byte: presence's 0x02 among them.
    std::string text = chat::live_chat_room("show-1")->to_string();
    for (unsigned tag = 0; tag <= 0xFF; ++tag) {
        if (tag == 0x01) {
            continue;
        }
        text.replace(0, 2, std::format("{:02x}", tag));
        const auto room = core::RoomId::parse(text);
        ASSERT_TRUE(room) << text;
        EXPECT_FALSE(is_stream_chat(*room)) << text;
    }
    const auto presence = core::RoomId::parse("021b9ed0-d6b6-88e6-ac34-32d7070ba83b");
    EXPECT_TRUE(core::ports::is_named_room(*presence, core::ports::NamedRoom::Presence));
    EXPECT_FALSE(core::ports::is_named_room(*presence, core::ports::NamedRoom::StreamChat));
}

TEST(StreamName, TakesWhatThePackagerTakesAndNothingElse) {
    EXPECT_TRUE(chat::is_stream_name("Show_2026-09-29"));
    EXPECT_TRUE(chat::is_stream_name(std::string(64, 'a')));
    EXPECT_FALSE(chat::is_stream_name(std::string(65, 'a')));
    EXPECT_FALSE(chat::is_stream_name(""));
    for (const char* bad : {"show/1", "show 1", "../show", "show.1", "sh\xc3\xb6w"}) {
        EXPECT_FALSE(chat::is_stream_name(bad)) << bad;
    }
}

} // namespace

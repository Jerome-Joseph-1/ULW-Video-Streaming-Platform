#include "core/ports/message_store.hpp"

#include "live_chat.hpp"

#include <gtest/gtest.h>
#include <string>

namespace {

using core::ports::is_stream_chat;

TEST(LiveChatRoom, IsTheDocumentedDigestOfTheStreamName) {
    // SHA-256("ulw-live-chat:show-1"), first 16 bytes, version 8 and the RFC 9562 variant: what
    // docs/integration/chat.md tells a client to compute, and live_chat_room() in SQL gives.
    const auto room = chat::live_chat_room("show-1");
    ASSERT_TRUE(room);
    EXPECT_EQ(room->to_string(), "1b9ed0d6-b6e8-86ac-b432-d7070ba83b94");
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
    EXPECT_FALSE(is_stream_chat(*core::RoomId::parse("1b9ed0d6-b6e8-46ac-b432-d7070ba83b94")));
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

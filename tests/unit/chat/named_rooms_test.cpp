#include "core/ports/message_store.hpp"

#include "live_chat.hpp"
#include "named_rooms.hpp"
#include "presence_room.hpp"

#include <gtest/gtest.h>
#include <string>

namespace {

using core::ports::NamedRoom;
using core::ports::RoomKind;

core::UserId user(std::string_view id) {
    return *core::UserId::parse(id);
}

rt::MessageKey request(std::string_view id) {
    return *rt::MessageKey::parse(id);
}

// The documented digests (docs/integration/chat.md): pinned, since every node, and every client
// that computes them, must agree on them for as long as the rooms exist.
TEST(NamedRooms, AreTheDocumentedDigests) {
    EXPECT_EQ(chat::direct_room(user("alice"), user("bob")).to_string(),
              "032768cd-63d3-8415-bc35-024bab6c3653");
    EXPECT_EQ(chat::group_room(user("alice"), request("g1")).to_string(),
              "044de777-5db4-83fe-8e78-4e00c64e4998");
}

TEST(NamedRooms, APairNamesOneDirectChatWhicheverOfThemAsks) {
    const core::RoomId room = chat::direct_room(user("alice"), user("bob"));
    EXPECT_EQ(chat::direct_room(user("bob"), user("alice")), room);
    EXPECT_NE(chat::direct_room(user("alice"), user("carol")), room);
    // The ids are not run together: "ab"+"c" is not "a"+"bc".
    EXPECT_NE(chat::direct_room(user("ab"), user("c")), chat::direct_room(user("a"), user("bc")));
    EXPECT_TRUE(core::ports::is_named_room(room, NamedRoom::DirectChat));
    EXPECT_EQ(core::ports::named_kind(room), RoomKind::DirectChat);
    EXPECT_FALSE(core::ports::is_stream_chat(room));
    EXPECT_FALSE(chat::is_presence_room(room));
}

TEST(NamedRooms, ACreatorsRequestNamesOneGroupChat) {
    const core::RoomId room = chat::group_room(user("alice"), request("g1"));
    EXPECT_EQ(chat::group_room(user("alice"), request("g1")), room);
    EXPECT_NE(chat::group_room(user("alice"), request("g2")), room);
    EXPECT_NE(chat::group_room(user("bob"), request("g1")), room);
    EXPECT_EQ(core::ports::named_kind(room), RoomKind::GroupChat);
    // Never the direct chat of the same text.
    EXPECT_NE(chat::direct_room(user("alice"), user("g1")), room);
}

TEST(NamedRooms, OnlyTheirTagsNameAKindAndEveryOtherRoomNamesNone) {
    EXPECT_EQ(core::ports::named_kind(*chat::live_chat_room("show-1")), std::nullopt);
    EXPECT_EQ(core::ports::named_kind(chat::presence_room(user("alice"))), std::nullopt);
    EXPECT_EQ(core::ports::named_kind(*core::RoomId::parse("03a0eb86-6cca-7dce-84cc-3bb47615f9fd")),
              std::nullopt)
        << "a version 7 id is never named";
    EXPECT_EQ(core::ports::named_kind(*core::RoomId::parse("04a0eb86-6cca-8dce-84cc-3bb47615f9fd")),
              RoomKind::GroupChat);
}

} // namespace

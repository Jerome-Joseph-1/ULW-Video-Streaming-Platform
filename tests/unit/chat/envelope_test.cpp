#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include "envelope.hpp"
#include "live_chat.hpp"
#include "named_rooms.hpp"
#include "presence_room.hpp"

#include <algorithm>
#include <format>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <variant>

namespace {

using chat::EnvelopeError;

constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";

core::RoomId room() {
    return *core::RoomId::parse(kRoom);
}

TEST(Envelope, AJoinNamesItsRoomAndMayAskToResumeAndToBeLossy) {
    const auto c =
        chat::parse_command(R"({"type":"join","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd"})");
    ASSERT_TRUE(c);
    const auto& join = std::get<chat::Join>(*c);
    EXPECT_EQ(join.room, room());
    EXPECT_FALSE(join.after);
    EXPECT_EQ(join.delivery, chat::Delivery::Durable);

    const auto resume = chat::parse_command(
        R"({"type":"join","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","after":41,"delivery":"lossy"})");
    ASSERT_TRUE(resume);
    EXPECT_EQ(std::get<chat::Join>(*resume).after, 41U);
    EXPECT_EQ(std::get<chat::Join>(*resume).delivery, chat::Delivery::Lossy);
    EXPECT_EQ(join.kind, core::ports::RoomKind::GroupChat);

    for (const auto& [text, kind] : {std::pair{"direct", core::ports::RoomKind::DirectChat},
                                     std::pair{"group", core::ports::RoomKind::GroupChat}}) {
        const auto named = chat::parse_command(
            std::string{
                R"({"type":"join","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","kind":")"} +
            text + R"("})");
        ASSERT_TRUE(named) << text;
        EXPECT_EQ(std::get<chat::Join>(*named).kind, kind) << text;
    }
    // A live chat is joined by its stream (ADR-0070), so a room id cannot ask to be one.
    for (const char* text : {"open", "live"}) {
        EXPECT_EQ(
            chat::parse_command(
                std::string{
                    R"({"type":"join","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","kind":")"} +
                text + R"("})"),
            std::unexpected(EnvelopeError::Malformed))
            << text;
    }
}

TEST(Envelope, AHistoryPageRunsBackFromTheNewestUnlessGivenACursor) {
    const auto newest =
        chat::parse_command(R"({"type":"history","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd"})");
    ASSERT_TRUE(newest);
    const auto& h = std::get<chat::History>(*newest);
    EXPECT_EQ(h.room, room());
    EXPECT_FALSE(h.before);
    EXPECT_FALSE(h.after);
    EXPECT_EQ(h.limit, chat::kDefaultHistoryLimit);

    const auto back = chat::parse_command(
        R"({"type":"history","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","before":90,"limit":100})");
    ASSERT_TRUE(back);
    EXPECT_EQ(std::get<chat::History>(*back).before, 90U);
    EXPECT_EQ(std::get<chat::History>(*back).limit, 100U);

    const auto forth = chat::parse_command(
        R"({"type":"history","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","after":0,"limit":1})");
    ASSERT_TRUE(forth);
    EXPECT_EQ(std::get<chat::History>(*forth).after, 0U);
}

TEST(Envelope, AHistoryPageWithBothCursorsOrALimitOutOfRangeIsMalformed) {
    for (const std::string_view extra :
         {R"(,"before":9,"after":2)", R"(,"limit":0)", R"(,"limit":101)", R"(,"after":-1)",
          R"(,"before":"9")", R"(,"from":9)"}) {
        const std::string text =
            std::string{R"({"type":"history","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd")"} +
            std::string{extra} + "}";
        EXPECT_EQ(chat::parse_command(text), std::unexpected(EnvelopeError::Malformed)) << text;
    }
}

TEST(Envelope, AStreamsLiveChatIsJoinedByTheStreamsNameAndNeverByItsRoom) {
    const auto c = chat::parse_command(R"({"type":"join","stream":"show-1","after":3})");
    ASSERT_TRUE(c);
    EXPECT_EQ(std::get<chat::Join>(*c).room, *chat::live_chat_room("show-1"));
    EXPECT_EQ(std::get<chat::Join>(*c).after, 3U);
    EXPECT_EQ(std::get<chat::Join>(*c).kind, core::ports::RoomKind::StreamLiveChat);

    const std::string live = chat::live_chat_room("show-1")->to_string();
    EXPECT_EQ(chat::parse_command(R"({"type":"join","room":")" + live + R"("})"),
              std::unexpected(EnvelopeError::BadRoom));
    EXPECT_EQ(chat::parse_command(R"({"type":"join","stream":"show/1"})"),
              std::unexpected(EnvelopeError::BadStream));
    EXPECT_EQ(chat::parse_command(R"({"type":"join","stream":""})"),
              std::unexpected(EnvelopeError::BadStream));
    EXPECT_EQ(chat::parse_command(R"({"type":"join","stream":7})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"join","stream":"show-1","kind":"live"})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"join","stream":"show-1","room":")" +
                                  std::string(kRoom) + R"("})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::reason(EnvelopeError::BadStream), "bad_stream");
    // Once joined, the room is named by its id like any other.
    const auto send =
        chat::parse_command(R"({"type":"send","room":")" + live + R"(","id":"a","body":"aGk"})");
    ASSERT_TRUE(send);
    EXPECT_EQ(std::get<chat::Send>(*send).room, *chat::live_chat_room("show-1"));
}

TEST(Envelope, ASendCarriesItsIdAndTheBytesItsBodyEncodes) {
    const std::string bytes("\x00\xff opaque \xc3", 11);
    const auto c = chat::parse_command(R"({"id":"0f4c2a9e-5b1d","type":"send","body":")" +
                                       infra::auth::encode_base64url(bytes) +
                                       R"(","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd"})");
    ASSERT_TRUE(c);
    const auto& send = std::get<chat::Send>(*c);
    EXPECT_EQ(send.room, room());
    EXPECT_EQ(send.id.view(), "0f4c2a9e-5b1d");
    const auto expected = std::as_bytes(std::span{bytes});
    EXPECT_TRUE(std::ranges::equal(send.body, expected));

    const auto empty = chat::parse_command(
        R"({"type":"send","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","id":"a","body":""})");
    ASSERT_TRUE(empty);
    EXPECT_TRUE(std::get<chat::Send>(*empty).body.empty());
}

TEST(Envelope, WhatIsNotACommandIsRefusedWithAReason) {
    const std::string room = R"("room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd")";
    EXPECT_EQ(chat::parse_command("{"), std::unexpected(EnvelopeError::NotJson));
    EXPECT_EQ(chat::parse_command("[]"), std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"part",)" + room + "}"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command("{" + room + "}"), std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"join"})"), std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(
        chat::parse_command(R"({"type":"join","room":"01A0EB86-6CCA-7DCE-84CC-3BB47615F9FD"})"),
        std::unexpected(EnvelopeError::BadRoom));
    EXPECT_EQ(chat::parse_command(R"({"type":"join",)" + room + R"(,"after":-1})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"join",)" + room + R"(,"delivery":"best"})"),
              std::unexpected(EnvelopeError::Malformed));
    // Every send names its id and its body.
    EXPECT_EQ(chat::parse_command(R"({"type":"send",)" + room + R"(,"body":""})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"send",)" + room + R"(,"id":"a"})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"send",)" + room + R"(,"id":7,"body":""})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"send",)" + room + R"(,"id":"a b","body":""})"),
              std::unexpected(EnvelopeError::BadId));
    EXPECT_EQ(chat::parse_command(R"({"type":"send",)" + room + R"(,"id":"a","body":"a+b/"})"),
              std::unexpected(EnvelopeError::BadBody));
    EXPECT_EQ(chat::parse_command(R"({"type":"send",)" + room + R"(,"id":"a","body":"aGk="})"),
              std::unexpected(EnvelopeError::BadBody));
    // A misspelt field is refused, not ignored; so is the M16 "ref".
    EXPECT_EQ(chat::parse_command(R"({"type":"send",)" + room + R"(,"id":"a","body":"","ref":1})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"join",)" + room + R"(,"afterr":1})"),
              std::unexpected(EnvelopeError::Malformed));
    // A second copy of a field is refused by the parser.
    EXPECT_EQ(chat::parse_command(R"({"type":"join","type":"send",)" + room + "}"),
              std::unexpected(EnvelopeError::NotJson));
}

TEST(Envelope, WatchAndUnwatchNameAUser) {
    const auto w = chat::parse_command(R"({"type":"watch","user":"auth0|bob"})");
    ASSERT_TRUE(w);
    EXPECT_EQ(std::get<chat::Watch>(*w).user.view(), "auth0|bob");
    const auto u = chat::parse_command(R"({"user":"auth0|bob","type":"unwatch"})");
    ASSERT_TRUE(u);
    EXPECT_EQ(std::get<chat::Unwatch>(*u).user.view(), "auth0|bob");

    EXPECT_EQ(chat::parse_command(R"({"type":"watch"})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"watch","user":7})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"watch","user":"bob","room":"x"})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"watch","user":"bob smith"})"),
              std::unexpected(EnvelopeError::BadUser));
    EXPECT_EQ(chat::parse_command(R"({"type":"unwatch","user":""})"),
              std::unexpected(EnvelopeError::BadUser));
}

TEST(Envelope, APresenceRoomCannotBeJoinedSentToOrReadAsAChatRoom) {
    const std::string presence = chat::presence_room(*core::UserId::parse("bob")).to_string();
    EXPECT_EQ(chat::parse_command(R"({"type":"join","room":")" + presence + R"("})"),
              std::unexpected(EnvelopeError::BadRoom));
    EXPECT_EQ(
        chat::parse_command(R"({"type":"send","room":")" + presence + R"(","id":"a","body":""})"),
        std::unexpected(EnvelopeError::BadRoom));
    EXPECT_EQ(chat::parse_command(R"({"type":"history","room":")" + presence + R"("})"),
              std::unexpected(EnvelopeError::BadRoom));
}

TEST(Envelope, AMessageReturnsItsBodyBytesExactlyAndItsId) {
    const std::string body("line one\n\"two\"\\ \x7f \x00\xfe", 20);
    const auto sender = *core::UserId::parse("auth0|alice");
    std::string out;
    chat::write_message(out, {.room = room(),
                              .seq = 9,
                              .sender = sender,
                              .key = *rt::MessageKey::parse("k_1-A"),
                              .body = std::as_bytes(std::span{body})});
    const auto json = core::json::parse(out);
    ASSERT_TRUE(json) << out;
    EXPECT_EQ(json->find("type")->as_string(), "message");
    EXPECT_EQ(json->find("room")->as_string(), kRoom);
    EXPECT_EQ(json->find("seq")->as_u64(), 9U);
    EXPECT_EQ(json->find("sender")->as_string(), "auth0|alice");
    EXPECT_EQ(json->find("id")->as_string(), "k_1-A");
    const auto encoded = json->find("body")->as_string();
    ASSERT_TRUE(encoded);
    EXPECT_EQ(infra::auth::decode_base64url(*encoded), body);
    // Nothing of the body shows through as text.
    EXPECT_EQ(out.find("line one"), std::string::npos);
}

TEST(Envelope, AMessagesWireSizeBoundsWhatWriteMessageMakes) {
    const std::string longest_sender(core::UserId::kMaxLength, 's');
    const std::string longest_id(rt::MessageKey::kMaxLength, 'i');
    for (const std::size_t size :
         {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{48} * 1024}) {
        const std::string body(size, 'b');
        std::string out;
        chat::write_message(out, {.room = room(),
                                  .seq = UINT64_MAX,
                                  .sender = *core::UserId::parse(longest_sender),
                                  .key = *rt::MessageKey::parse(longest_id),
                                  .body = std::as_bytes(std::span{body})});
        // The frame header of a text message under 64 KiB is 4 bytes.
        EXPECT_LE(out.size() + 4, chat::message_wire_size(size)) << size;
        EXPECT_GE(out.size() + 4 + 8, chat::message_wire_size(size)) << size;
    }
}

TEST(Envelope, AMessageWrittenIntoAReusedTextIsTheSameAsIntoANewOne) {
    const auto sender = *core::UserId::parse("auth0|alice");
    const auto message = [&](const std::string& body) {
        return rt::Message{.room = room(),
                           .seq = 3,
                           .sender = sender,
                           .key = *rt::MessageKey::parse("k"),
                           .body = std::as_bytes(std::span{body})};
    };
    std::string reused;
    for (const std::size_t size : {std::size_t{48} * 1024, std::size_t{1}, std::size_t{0},
                                   std::size_t{30} * 1024, std::size_t{2}}) {
        const std::string body(size, static_cast<char>('a' + (size % 26)));
        std::string fresh;
        chat::write_message(fresh, message(body));
        reused.clear();
        chat::write_message(reused, message(body));
        EXPECT_EQ(reused, fresh) << size;
        EXPECT_LE(fresh.size() + 4, chat::message_wire_size(size)) << size;
    }
}

TEST(Envelope, ACallNamesItsRoomAndTheAskingDevice) {
    const auto c =
        chat::parse_command(R"({"type":"call","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd",)"
                            R"("device":"01a0eb86-6cca-7dce-84cc-3bb47615f9aa"})");
    ASSERT_TRUE(c);
    const auto& call = std::get<chat::Call>(*c);
    EXPECT_EQ(call.room, room());
    EXPECT_EQ(call.device.to_string(), "01a0eb86-6cca-7dce-84cc-3bb47615f9aa");
    const std::string room_field = R"("room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd")";
    EXPECT_EQ(chat::parse_command(R"({"type":"call",)" + room_field + "}"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"call",)" + room_field + R"(,"device":"phone"})"),
              std::unexpected(EnvelopeError::BadDevice));
    EXPECT_EQ(
        chat::parse_command(R"({"type":"call",)" + room_field +
                            R"(,"device":"01a0eb86-6cca-7dce-84cc-3bb47615f9aa","generation":2})"),
        std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(
        chat::parse_command(R"({"type":"call","device":"01a0eb86-6cca-7dce-84cc-3bb47615f9aa"})"),
        std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::reason(EnvelopeError::BadDevice), "bad_device");
}

TEST(Envelope, RepliesAreTheDocumentedShapes) {
    const auto id = *rt::MessageKey::parse("m-3");
    std::string out;
    chat::write_joined(out, room(), 12);
    EXPECT_EQ(out, R"({"type":"joined","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","seq":12})");
    out.clear();
    chat::write_sent(out, room(), id, 17);
    EXPECT_EQ(
        out,
        R"({"type":"sent","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","id":"m-3","seq":17})");
    out.clear();
    chat::write_error(out, chat::reason(rt::RouteError::Fenced), room(), id);
    EXPECT_EQ(
        out,
        R"({"type":"error","reason":"fenced","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","id":"m-3"})");
    out.clear();
    chat::write_rate_limited(out, room(), id, core::Millis{500});
    EXPECT_EQ(
        out,
        R"({"type":"error","reason":"rate_limited","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","id":"m-3","retry_after_ms":500})");
    out.clear();
    chat::write_history(out, room(), 3);
    EXPECT_EQ(out, R"({"type":"history","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","count":3})");
    out.clear();
    chat::write_error(out, chat::reason(EnvelopeError::NotJson));
    EXPECT_EQ(out, R"({"type":"error","reason":"not_json"})");
    const auto bob = *core::UserId::parse("auth0|bob");
    out.clear();
    chat::write_presence(out, "watching", bob, false);
    EXPECT_EQ(out, R"({"type":"watching","user":"auth0|bob","status":"offline"})");
    out.clear();
    chat::write_presence(out, "presence", bob, true);
    EXPECT_EQ(out, R"({"type":"presence","user":"auth0|bob","status":"online"})");
    out.clear();
    chat::write_user_error(out, "too_many_watches", bob);
    EXPECT_EQ(out, R"({"type":"error","reason":"too_many_watches","user":"auth0|bob"})");
    out.clear();
    chat::write_ticket(
        out, room(),
        {.endpoint = "wss://media.example.test",
         .credential = "eyJ.a.b",
         .expires_at = core::WallTime{std::chrono::milliseconds{1'790'000'060'999}}});
    EXPECT_EQ(
        out,
        R"({"type":"ticket","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","url":"wss://media.example.test","token":"eyJ.a.b","expires_at":1790000060})");
    out.clear();
    chat::write_call_error(out, "unavailable", room(), core::Millis{2000});
    EXPECT_EQ(
        out,
        R"({"type":"error","reason":"unavailable","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","retry_after_ms":2000})");
    out.clear();
    chat::write_call_error(out, "not_callable", room(), std::nullopt);
    EXPECT_EQ(
        out,
        R"({"type":"error","reason":"not_callable","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd"})");
}

// Member lists (ADR-0096).

core::UserId uid(std::string_view id) {
    return *core::UserId::parse(id);
}

TEST(Envelope, MemberListCommandsCarryWhatTheyName) {
    const auto open = chat::parse_command(R"({"type":"open_direct","user":"auth0|bob"})");
    ASSERT_TRUE(open);
    EXPECT_EQ(std::get<chat::OpenDirect>(*open).user, uid("auth0|bob"));

    const auto create =
        chat::parse_command(R"({"type":"create_group","id":"g-1","users":["bob","carol","bob"]})");
    ASSERT_TRUE(create);
    const auto& group = std::get<chat::CreateGroup>(*create);
    EXPECT_EQ(group.id.view(), "g-1");
    // As given: the service takes out repeats and the asker.
    EXPECT_EQ(group.users, (std::vector<core::UserId>{uid("bob"), uid("carol"), uid("bob")}));
    const auto alone = chat::parse_command(R"({"type":"create_group","id":"g-2"})");
    ASSERT_TRUE(alone);
    EXPECT_TRUE(std::get<chat::CreateGroup>(*alone).users.empty());
    ASSERT_TRUE(chat::parse_command(R"({"type":"create_group","id":"g-3","users":[]})"));

    const std::string room = R"("room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd")";
    const auto add = chat::parse_command(R"({"type":"add_members",)" + room + R"(,"users":["d"]})");
    ASSERT_TRUE(add);
    EXPECT_EQ(std::get<chat::AddMembers>(*add).room, ::room());
    EXPECT_EQ(std::get<chat::AddMembers>(*add).users, std::vector<core::UserId>{uid("d")});

    const auto remove =
        chat::parse_command(R"({"type":"remove_member",)" + room + R"(,"user":"d"})");
    ASSERT_TRUE(remove);
    EXPECT_EQ(std::get<chat::RemoveMember>(*remove).user, uid("d"));

    const auto leave = chat::parse_command(R"({"type":"leave",)" + room + "}");
    ASSERT_TRUE(leave);
    EXPECT_EQ(std::get<chat::LeaveRoom>(*leave).room, ::room());

    const auto rooms = chat::parse_command(R"({"type":"rooms"})");
    ASSERT_TRUE(rooms);
    EXPECT_EQ(std::get<chat::ListRooms>(*rooms).after, std::nullopt);
    EXPECT_EQ(std::get<chat::ListRooms>(*rooms).limit, chat::kDefaultListLimit);
    const auto paged = chat::parse_command(
        R"({"type":"rooms","after":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","limit":100})");
    ASSERT_TRUE(paged);
    EXPECT_EQ(std::get<chat::ListRooms>(*paged).after, ::room());
    EXPECT_EQ(std::get<chat::ListRooms>(*paged).limit, 100U);

    const auto members =
        chat::parse_command(R"({"type":"members",)" + room + R"(,"after":"bob","limit":1})");
    ASSERT_TRUE(members);
    EXPECT_EQ(std::get<chat::ListMembers>(*members).after, uid("bob"));
    EXPECT_EQ(std::get<chat::ListMembers>(*members).limit, 1U);
    const auto first = chat::parse_command(R"({"type":"members",)" + room + "}");
    ASSERT_TRUE(first);
    EXPECT_EQ(std::get<chat::ListMembers>(*first).after, std::nullopt);
}

TEST(Envelope, MemberListCommandsRefuseWhatTheyDoNotDefine) {
    const std::string room = R"("room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd")";
    const auto refused = [](const std::string& text) {
        const auto c = chat::parse_command(text);
        return c ? std::optional<EnvelopeError>{} : std::optional(c.error());
    };
    EXPECT_EQ(refused(R"({"type":"open_direct"})"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"open_direct","user":"a b"})"), EnvelopeError::BadUser);
    EXPECT_EQ(refused(R"({"type":"open_direct","user":"bob","room":"x"})"),
              EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"create_group","users":["bob"]})"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"create_group","id":"a b"})"), EnvelopeError::BadId);
    EXPECT_EQ(refused(R"({"type":"create_group","id":"g","users":"bob"})"),
              EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"create_group","id":"g","users":[1]})"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"create_group","id":"g","users":["a b"]})"),
              EnvelopeError::BadUser);
    std::string fifty_one = R"({"type":"create_group","id":"g","users":[)";
    for (int i = 0; i < 51; ++i) {
        fifty_one += std::format(R"({}"u{}")", i == 0 ? "" : ",", i);
    }
    EXPECT_EQ(refused(fifty_one + "]}"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"add_members",)" + room + R"(,"users":[]})"),
              EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"add_members",)" + room + "}"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"add_members","users":["d"]})"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"remove_member",)" + room + "}"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"remove_member","room":"x","user":"d"})"), EnvelopeError::BadRoom);
    EXPECT_EQ(refused(R"({"type":"leave",)" + room + R"(,"user":"d"})"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"rooms","limit":0})"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"rooms","limit":101})"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"rooms","after":"x"})"), EnvelopeError::BadRoom);
    EXPECT_EQ(refused(R"({"type":"rooms","after":1})"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"rooms","room":"x"})"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"members",)" + room + R"(,"after":"a b"})"),
              EnvelopeError::BadUser);
    EXPECT_EQ(refused(R"({"type":"members",)" + room + R"(,"after":7})"), EnvelopeError::Malformed);
    EXPECT_EQ(refused(R"({"type":"members",)" + room + R"(,"limit":"5"})"),
              EnvelopeError::Malformed);
    // A presence room is never one of these either.
    const std::string presence = chat::presence_room(uid("bob")).to_string();
    EXPECT_EQ(refused(R"({"type":"leave","room":")" + presence + R"("})"), EnvelopeError::BadRoom);
}

// A room named by a pair or a creator is the kind its id names: a join asks for it whatever it
// says, and one that names the other kind names the wrong room.
TEST(Envelope, AJoinOfANamedRoomAsksForTheKindItsIdNames) {
    const std::string direct = chat::direct_room(uid("alice"), uid("bob")).to_string();
    const std::string group =
        chat::group_room(uid("alice"), *rt::MessageKey::parse("g")).to_string();
    const auto join = [](const std::string& text) { return chat::parse_command(text); };
    const auto plain = join(R"({"type":"join","room":")" + direct + R"("})");
    ASSERT_TRUE(plain);
    EXPECT_EQ(std::get<chat::Join>(*plain).kind, core::ports::RoomKind::DirectChat);
    const auto said = join(R"({"type":"join","room":")" + direct + R"(","kind":"direct"})");
    ASSERT_TRUE(said);
    EXPECT_EQ(std::get<chat::Join>(*said).kind, core::ports::RoomKind::DirectChat);
    EXPECT_EQ(join(R"({"type":"join","room":")" + direct + R"(","kind":"group"})"),
              std::unexpected(EnvelopeError::BadRoom));
    const auto grouped = join(R"({"type":"join","room":")" + group + R"(","kind":"group"})");
    ASSERT_TRUE(grouped);
    EXPECT_EQ(std::get<chat::Join>(*grouped).kind, core::ports::RoomKind::GroupChat);
    EXPECT_EQ(join(R"({"type":"join","room":")" + group + R"(","kind":"direct"})"),
              std::unexpected(EnvelopeError::BadRoom));
}

TEST(Envelope, MemberListRepliesAreTheDocumentedShapes) {
    const std::string r = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
    std::string out;
    chat::write_direct(out, room(), uid("bob"));
    EXPECT_EQ(out, R"({"type":"direct","room":")" + r + R"(","user":"bob"})");
    out.clear();
    chat::write_group(out, room(), *rt::MessageKey::parse("g-1"));
    EXPECT_EQ(out, R"({"type":"group","room":")" + r + R"(","id":"g-1"})");
    out.clear();
    const std::vector<core::UserId> users{uid("bob"), uid("carol")};
    chat::write_added(out, room(), users);
    EXPECT_EQ(out, R"({"type":"added","room":")" + r + R"(","users":["bob","carol"]})");
    out.clear();
    chat::write_added(out, room(), {});
    EXPECT_EQ(out, R"({"type":"added","room":")" + r + R"(","users":[]})");
    out.clear();
    chat::write_removed(out, room(), uid("bob"));
    EXPECT_EQ(out, R"({"type":"removed","room":")" + r + R"(","user":"bob"})");
    out.clear();
    chat::write_left(out, room());
    EXPECT_EQ(out, R"({"type":"left","room":")" + r + R"("})");
    out.clear();
    const std::vector<core::ports::RoomEntry> rooms{{.room = room(),
                                                     .kind = core::ports::RoomKind::DirectChat,
                                                     .role = core::ports::MemberRole::Member,
                                                     .peer = uid("bob")},
                                                    {.room = room(),
                                                     .kind = core::ports::RoomKind::GroupChat,
                                                     .role = core::ports::MemberRole::Admin,
                                                     .peer = std::nullopt},
                                                    {.room = room(),
                                                     .kind = core::ports::RoomKind::StreamLiveChat,
                                                     .role = core::ports::MemberRole::Member,
                                                     .peer = std::nullopt}};
    chat::write_rooms(out, rooms, true);
    EXPECT_EQ(out, R"({"type":"rooms","rooms":[{"room":")" + r +
                       R"(","kind":"direct","role":"member","peer":"bob"},{"room":")" + r +
                       R"(","kind":"group","role":"admin"},{"room":")" + r +
                       R"(","kind":"live","role":"member"}],"more":true})");
    out.clear();
    chat::write_rooms(out, {}, false);
    EXPECT_EQ(out, R"({"type":"rooms","rooms":[],"more":false})");
    out.clear();
    const std::vector<core::ports::MemberEntry> members{
        {.user = uid("alice"), .role = core::ports::MemberRole::Admin},
        {.user = uid("bob"), .role = core::ports::MemberRole::Member}};
    chat::write_members(out, room(), members, false);
    EXPECT_EQ(
        out,
        R"({"type":"members","room":")" + r +
            R"(","members":[{"user":"alice","role":"admin"},{"user":"bob","role":"member"}],"more":false})");
    out.clear();
    chat::write_member_change(out, room(), uid("bob"), true);
    EXPECT_EQ(out, R"({"type":"member","room":")" + r + R"(","user":"bob","change":"added"})");
    out.clear();
    chat::write_member_change(out, room(), uid("bob"), false);
    EXPECT_EQ(out, R"({"type":"member","room":")" + r + R"(","user":"bob","change":"removed"})");
    out.clear();
    chat::write_error_with(out, "rate_limited",
                           {.room = std::nullopt,
                            .id = *rt::MessageKey::parse("g-1"),
                            .user = uid("bob"),
                            .retry_after = core::Millis{2500}});
    EXPECT_EQ(
        out,
        R"({"type":"error","reason":"rate_limited","id":"g-1","user":"bob","retry_after_ms":2500})");
    out.clear();
    chat::write_error_with(
        out, "not_admin",
        {.room = room(), .id = std::nullopt, .user = std::nullopt, .retry_after = std::nullopt});
    EXPECT_EQ(out, R"({"type":"error","reason":"not_admin","room":")" + r + R"("})");
}

} // namespace

#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include "envelope.hpp"
#include "live_chat.hpp"
#include "presence_room.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <initializer_list>
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
    EXPECT_EQ(chat::parse_command(R"({"type":"leave",)" + room + "}"),
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

TEST(Envelope, ACallMayNameTheRingingCallItAnswers) {
    const std::string head = R"({"type":"call","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd",)"
                             R"("device":"01a0eb86-6cca-7dce-84cc-3bb47615f9aa")";
    const auto c =
        chat::parse_command(head + R"(,"answer":"01a0f3c2-55d1-7e2a-9b3c-4d5e6f708192"})");
    ASSERT_TRUE(c);
    const auto& call = std::get<chat::Call>(*c);
    ASSERT_TRUE(call.answering);
    EXPECT_EQ(call.answering->to_string(), "01a0f3c2-55d1-7e2a-9b3c-4d5e6f708192");
    const auto plain = chat::parse_command(head + "}");
    ASSERT_TRUE(plain);
    EXPECT_FALSE(std::get<chat::Call>(*plain).answering);
    EXPECT_EQ(chat::parse_command(head + R"(,"answer":"ringing"})"),
              std::unexpected(EnvelopeError::BadCall));
    EXPECT_EQ(chat::parse_command(head + R"(,"answer":7})"),
              std::unexpected(EnvelopeError::Malformed));
}

TEST(Envelope, ADeclineCancelOrEndNamesItsRoomAndCall) {
    const std::string room_field = R"("room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd")";
    const std::string call_field = R"("call":"01a0eb86-6cca-7dce-84cc-3bb47615f9cc")";
    const std::vector<std::pair<std::string, chat::CallSignal>> moves{
        {"call_decline", chat::CallSignal::Decline},
        {"call_cancel", chat::CallSignal::Cancel},
        {"call_end", chat::CallSignal::End},
        {"call_leave", chat::CallSignal::Leave}};
    // The command `type` with `fields` after its type.
    const auto command = [](const std::string& type,
                            std::initializer_list<std::string_view> fields) {
        std::string out = R"({"type":")";
        out += type;
        out += '"';
        for (const std::string_view f : fields) {
            out += ',';
            out += f;
        }
        out += '}';
        return out;
    };
    for (const auto& [type, signal] : moves) {
        const auto c = chat::parse_command(command(type, {room_field, call_field}));
        ASSERT_TRUE(c) << type;
        const auto& move = std::get<chat::CallMove>(*c);
        EXPECT_EQ(move.room, room());
        EXPECT_EQ(move.signal, signal);
        EXPECT_EQ(move.call.to_string(), "01a0eb86-6cca-7dce-84cc-3bb47615f9cc");
        EXPECT_EQ(chat::parse_command(command(type, {room_field})),
                  std::unexpected(EnvelopeError::Malformed));
        EXPECT_EQ(chat::parse_command(command(type, {room_field, R"("call":"ring-1")"})),
                  std::unexpected(EnvelopeError::BadCall));
        EXPECT_EQ(chat::parse_command(command(type, {room_field, call_field, R"("device":"x")"})),
                  std::unexpected(EnvelopeError::Malformed));
        EXPECT_EQ(chat::parse_command(command(type, {R"("room":"lobby")", call_field})),
                  std::unexpected(EnvelopeError::BadRoom));
    }
    EXPECT_EQ(chat::reason(EnvelopeError::BadCall), "bad_call");
}

TEST(Envelope, AnExpulsionNamesItsRoomCallAndWhoIsPutOut) {
    const std::string fields = R"("room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd",)"
                               R"("call":"01a0eb86-6cca-7dce-84cc-3bb47615f9cc")";
    const auto c = chat::parse_command(R"({"type":"call_expel",)" + fields + R"(,"user":"bob"})");
    ASSERT_TRUE(c);
    const auto& move = std::get<chat::CallMove>(*c);
    EXPECT_EQ(move.signal, chat::CallSignal::Expel);
    EXPECT_EQ(move.target, core::UserId::parse("bob").value());
    EXPECT_EQ(chat::parse_command(R"({"type":"call_expel",)" + fields + "}"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"call_expel",)" + fields + R"(,"user":7})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"call_expel",)" + fields + R"(,"user":""})"),
              std::unexpected(EnvelopeError::BadUser));
    // Only an expulsion names a user.
    EXPECT_EQ(chat::parse_command(R"({"type":"call_leave",)" + fields + R"(,"user":"bob"})"),
              std::unexpected(EnvelopeError::Malformed));
    EXPECT_EQ(chat::parse_command(R"({"type":"call_expel","room":"lobby",)"
                                  R"("call":"01a0eb86-6cca-7dce-84cc-3bb47615f9cc","user":"bob"})"),
              std::unexpected(EnvelopeError::BadRoom));
    EXPECT_EQ(chat::parse_command(R"({"type":"call_expel",)"
                                  R"("room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd",)"
                                  R"("call":"x","user":"bob"})"),
              std::unexpected(EnvelopeError::BadCall));
}

TEST(Envelope, GroupCallEventsAreTheDocumentedShapes) {
    const ulw::test::FakeClock clock;
    ulw::test::FakeRandom random;
    const auto call = chat::CallId::generate(clock, random);
    const std::string head = R"("room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","call":")" +
                             call.to_string() + R"(","from":"alice")";
    const auto alice = *core::UserId::parse("alice");
    const auto bob = *core::UserId::parse("bob");
    std::string out;
    chat::write_call_event(out, {.event = chat::RingEvent::Left,
                                 .to = alice,
                                 .room = room(),
                                 .call = call,
                                 .from = alice,
                                 .by = bob,
                                 .expires_at = {}});
    EXPECT_EQ(out, R"({"type":"call_left",)" + head + R"(,"by":"bob"})");
    out.clear();
    chat::write_call_event(out, {.event = chat::RingEvent::Moved,
                                 .to = alice,
                                 .room = room(),
                                 .call = call,
                                 .from = alice,
                                 .by = alice,
                                 .expires_at = {},
                                 .subject = bob});
    EXPECT_EQ(out, R"({"type":"call_moved",)" + head + R"(,"by":"alice","expelled":"bob"})");
    // Put out by a removal from the chat, and a call nobody was left in: nobody did it.
    out.clear();
    chat::write_call_event(out, {.event = chat::RingEvent::Moved,
                                 .to = alice,
                                 .room = room(),
                                 .call = call,
                                 .from = alice,
                                 .by = std::nullopt,
                                 .expires_at = {},
                                 .subject = bob});
    EXPECT_EQ(out, R"({"type":"call_moved",)" + head + R"(,"expelled":"bob"})");
    out.clear();
    chat::write_call_event(out, {.event = chat::RingEvent::Ended,
                                 .to = alice,
                                 .room = room(),
                                 .call = call,
                                 .from = alice,
                                 .by = std::nullopt,
                                 .expires_at = {}});
    EXPECT_EQ(out, R"({"type":"call_ended",)" + head + "}");
}

TEST(Envelope, CallEventsAreTheDocumentedShapes) {
    const ulw::test::FakeClock clock;
    ulw::test::FakeRandom random;
    const auto call = chat::CallId::generate(clock, random);
    const std::string call_text = call.to_string();
    const auto alice = *core::UserId::parse("auth0|alice");
    const auto bob = *core::UserId::parse("auth0|bob");
    std::string out;
    chat::write_call_event(out, {.event = chat::RingEvent::Ringing,
                                 .to = bob,
                                 .room = room(),
                                 .call = call,
                                 .from = alice,
                                 .by = std::nullopt,
                                 .expires_at = core::WallTime{core::Millis{1'790'000'045'500}}});
    EXPECT_EQ(out,
              R"({"type":"call_ringing","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","call":")" +
                  call_text + R"(","from":"auth0|alice","expires_at":1790000045})");
    const std::vector<std::pair<chat::RingEvent, std::string>> ended{
        {chat::RingEvent::Answered, "call_answered"},
        {chat::RingEvent::Declined, "call_declined"},
        {chat::RingEvent::Cancelled, "call_cancelled"},
        {chat::RingEvent::Ended, "call_ended"}};
    for (const auto& [event, type] : ended) {
        out.clear();
        chat::write_call_event(out, {.event = event,
                                     .to = alice,
                                     .room = room(),
                                     .call = call,
                                     .from = alice,
                                     .by = bob,
                                     .expires_at = {}});
        std::string expected = R"({"type":")";
        expected += type;
        expected += R"(","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","call":")";
        expected += call_text;
        expected += R"(","from":"auth0|alice","by":"auth0|bob"})";
        EXPECT_EQ(out, expected);
    }
    out.clear();
    chat::write_call_event(out, {.event = chat::RingEvent::Missed,
                                 .to = alice,
                                 .room = room(),
                                 .call = call,
                                 .from = alice,
                                 .by = std::nullopt,
                                 .expires_at = {}});
    EXPECT_EQ(out,
              R"({"type":"call_missed","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","call":")" +
                  call_text + R"(","from":"auth0|alice"})");
    out.clear();
    chat::write_ticket(out, room(),
                       {.endpoint = "wss://m", .credential = "t", .expires_at = core::WallTime{}},
                       call);
    EXPECT_EQ(
        out,
        R"({"type":"ticket","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","url":"wss://m","token":"t","expires_at":0,"call":")" +
            call_text + R"("})");
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
    out.clear();
    chat::write_call_over(out, room(),
                          *chat::CallId::parse("01a0f3c2-55d1-7e2a-9b3c-4d5e6f708192"));
    EXPECT_EQ(
        out,
        R"({"type":"error","reason":"no_call","room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","call":"01a0f3c2-55d1-7e2a-9b3c-4d5e6f708192"})");
}

} // namespace

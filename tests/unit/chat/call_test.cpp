#include "call.hpp"
#include "support/fake_clock.hpp"
#include "support/no_membership_store.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using chat::CallOutcome;
using core::ports::MediaError;
using core::ports::RoomKind;

constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
constexpr std::string_view kOtherRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fe";
constexpr std::string_view kDevice = "01a0eb86-6cca-7dce-84cc-3bb47615f9aa";
constexpr std::string_view kOtherDevice = "01a0eb86-6cca-7dce-84cc-3bb47615f9bb";

core::RoomId room_id(std::string_view room = kRoom) {
    return *core::RoomId::parse(room);
}

// Only access matters to the handler; it answers what the test set, at once or when told to.
class FakeStore final : public ulw::test::NoMembershipStore {
public:
    void history_before(
        const core::RoomId& /*room*/, std::optional<std::uint64_t> /*before*/,
        std::size_t /*limit*/,
        core::ports::MessageCallback<std::vector<core::ports::StoredMessage>> done) override {
        done(std::vector<core::ports::StoredMessage>{});
    }
    void history_after(
        const core::RoomId& /*room*/, std::uint64_t /*after*/, std::size_t /*limit*/,
        core::ports::MessageCallback<std::vector<core::ports::StoredMessage>> done) override {
        done(std::vector<core::ports::StoredMessage>{});
    }
    void last_seq(const core::RoomId& /*room*/,
                  core::ports::MessageCallback<std::uint64_t> done) override {
        done(std::uint64_t{0});
    }
    void add_member(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                    core::ports::MessageCallback<void> done) override {
        done({});
    }
    void remove_member(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                       core::ports::MessageCallback<void> done) override {
        done({});
    }
    void members(const core::RoomId& /*room*/, std::optional<core::UserId> /*after*/,
                 std::size_t /*limit*/,
                 core::ports::MessageCallback<std::vector<core::UserId>> done) override {
        done(std::vector<core::UserId>{});
    }
    using core::ports::IMessageStore::admits;
    void admits(const core::RoomId& /*room*/, const core::UserId& /*user*/, RoomKind /*asked*/,
                core::ports::Recording /*recording*/,
                core::ports::MessageCallback<core::ports::Admission> done) override {
        done(core::ports::Admission::Admitted);
    }
    void access(const core::RoomId& /*room*/, const core::UserId& user,
                core::ports::MessageCallback<core::ports::RoomAccess> done) override {
        ++reads;
        if (throwing) {
            throw std::bad_alloc();
        }
        if (down) {
            done(std::unexpected(core::ports::MessageStoreError::Unavailable));
            return;
        }
        done(core::ports::RoomAccess{
            .kind = kind, .member = std::ranges::find(members_, user.view()) != members_.end()});
    }
    void record_live(const core::RoomId& /*room*/,
                     core::ports::MessageCallback<void> done) override {
        done({});
    }
    void watch_members(core::ports::IMemberListener* /*listener*/) noexcept override {}

    std::optional<RoomKind> kind = RoomKind::DirectChat;
    std::vector<std::string> members_{"alice", "bob"};
    bool down = false;
    // access fails to take the ask at all, as an allocation inside it would.
    bool throwing = false;
    int reads = 0;
};

struct Joined {
    std::string user;
    std::string device;
    core::ports::MediaRole role;
    core::ports::TicketDone done;
};

// The SFU, answered by hand: the test decides when a room opens and a ticket is issued.
class FakeSfu final : public core::ports::ISfu {
public:
    class Room final : public core::ports::IMediaRoom {
    public:
        explicit Room(FakeSfu& sfu) noexcept : sfu_(sfu) {}
        void join(const core::UserId& user, const core::DeviceId& device,
                  core::ports::MediaRole role, core::ports::TicketDone done) override {
            sfu_.joins.push_back({.user = std::string(user.view()),
                                  .device = device.to_string(),
                                  .role = role,
                                  .done = std::move(done)});
        }
        void participants(core::ports::ParticipantsDone done) override {
            done(std::unexpected(MediaError::NotImplemented));
        }
        void relay(const core::UserId& /*user*/, const core::DeviceId& /*device*/,
                   const core::ports::MediaRelay& /*target*/,
                   core::ports::RelayDone done) override {
            done(std::unexpected(MediaError::Refused));
        }
        void close(core::ports::MediaDone done) override {
            ++sfu_.closes;
            done({});
        }

    private:
        FakeSfu& sfu_;
    };

    struct Opening {
        core::RoomId room;
        core::ports::MediaGeneration generation;
        core::ports::MediaRoomKind kind;
        std::uint16_t max_participants;
        OpenDone done;
    };

    void open_room(const core::RoomId& room, core::ports::MediaGeneration generation,
                   core::ports::MediaRoomKind kind, std::uint16_t max_participants,
                   OpenDone done) override {
        if (throwing) {
            throw std::bad_alloc();
        }
        opens.push_back({.room = room,
                         .generation = generation,
                         .kind = kind,
                         .max_participants = max_participants,
                         .done = std::move(done)});
    }

    // Answers the oldest open.
    void open(std::expected<void, MediaError> result = {}) {
        Opening o = std::move(opens.front());
        opens.erase(opens.begin());
        ++answered_opens;
        if (!result) {
            o.done(std::unexpected(result.error()));
            return;
        }
        o.done(std::make_unique<Room>(*this));
    }

    // Answers the oldest join with a ticket for its participant, or the error.
    void issue(std::expected<void, MediaError> result = {}) {
        Joined j = std::move(joins.front());
        joins.erase(joins.begin());
        if (!result) {
            j.done(std::unexpected(result.error()));
            return;
        }
        j.done(core::ports::MediaTicket{.endpoint = "wss://media.test",
                                        .credential = "jwt-for-" + j.user + "/" + j.device,
                                        .expires_at =
                                            core::WallTime{std::chrono::seconds{1'790'000'060}}});
    }

    std::vector<Opening> opens;
    std::vector<Joined> joins;
    int answered_opens = 0;
    int closes = 0;
    // open_room fails to take the open at all, as an allocation inside it would.
    bool throwing = false;
};

class CallHandlerTest : public ::testing::Test {
protected:
    explicit CallHandlerTest(chat::CallLimits limits = {})
        : handler_(std::make_unique<chat::CallHandler>(store_, &sfu_, clock_, limits)) {}

    // Asks as `user` from `device`; the answer lands in answers_, in the order they come.
    void ask(std::string_view user = "alice", std::string_view device = kDevice,
             std::string_view room = kRoom) {
        const auto request = chat::encode_request(
            {.user = *core::UserId::parse(user), .device = *core::DeviceId::parse(device)});
        handler_->on_ask(room_id(room), request,
                         [this](std::expected<std::vector<std::byte>, rt::RouteError> r) noexcept {
                             if (!r) {
                                 errors_.push_back(r.error());
                                 return;
                             }
                             answers_.push_back(*chat::decode_answer(*r));
                         });
    }

    [[nodiscard]] std::vector<CallOutcome> outcomes() const {
        std::vector<CallOutcome> out;
        out.reserve(answers_.size());
        for (const chat::CallAnswer& a : answers_) {
            out.push_back(a.outcome);
        }
        return out;
    }

    FakeStore store_;
    FakeSfu sfu_;
    ulw::test::FakeClock clock_;
    std::unique_ptr<chat::CallHandler> handler_;
    std::vector<chat::CallAnswer> answers_;
    std::vector<rt::RouteError> errors_;
};

TEST_F(CallHandlerTest, AMemberOfADirectChatGetsATicketForItsDeviceInTheRoomsCall) {
    ask("alice", kDevice);
    ASSERT_EQ(sfu_.opens.size(), 1U);
    EXPECT_EQ(sfu_.opens[0].room, room_id());
    EXPECT_EQ(sfu_.opens[0].generation, chat::kCallGeneration);
    EXPECT_EQ(sfu_.opens[0].kind, core::ports::MediaRoomKind::Call);
    // 1:1: two participants at most (ADR-0050, ADR-0058).
    EXPECT_EQ(sfu_.opens[0].max_participants, 2U);
    sfu_.open();
    ASSERT_EQ(sfu_.joins.size(), 1U);
    EXPECT_EQ(sfu_.joins[0].user, "alice");
    EXPECT_EQ(sfu_.joins[0].device, kDevice);
    EXPECT_EQ(sfu_.joins[0].role, core::ports::MediaRole::Member);
    EXPECT_TRUE(answers_.empty());
    sfu_.issue();
    ASSERT_EQ(answers_.size(), 1U);
    EXPECT_EQ(answers_[0].outcome, CallOutcome::Ticket);
    ASSERT_TRUE(answers_[0].ticket);
    EXPECT_EQ(answers_[0].ticket->endpoint, "wss://media.test");
    EXPECT_EQ(answers_[0].ticket->credential, "jwt-for-alice/" + std::string(kDevice));
    EXPECT_EQ(answers_[0].ticket->expires_at, core::WallTime{std::chrono::seconds{1'790'000'060}});
    EXPECT_EQ(handler_->counters().tickets, 1U);
}

TEST_F(CallHandlerTest, SomeoneNotOnTheMemberListIsRefusedAndTheSfuNeverHearsOfIt) {
    ask("mallory");
    EXPECT_EQ(outcomes(), std::vector{CallOutcome::NotMember});
    EXPECT_TRUE(sfu_.opens.empty());
    EXPECT_EQ(handler_->counters().not_member, 1U);
}

TEST_F(CallHandlerTest, OnlyADirectChatHasACall) {
    for (const std::optional<RoomKind> kind :
         {std::optional<RoomKind>{RoomKind::GroupChat},
          std::optional<RoomKind>{RoomKind::StreamLiveChat}, std::optional<RoomKind>{}}) {
        store_.kind = kind;
        ask("alice");
    }
    EXPECT_EQ(outcomes(), std::vector(3, CallOutcome::NotCallable));
    EXPECT_TRUE(sfu_.opens.empty());
}

TEST_F(CallHandlerTest, TheRoomIsOpenedOnceAndItsHandleReusedForEveryLaterAsk) {
    ask("alice");
    sfu_.open();
    sfu_.issue();
    ask("bob", kOtherDevice);
    ask("alice");
    EXPECT_TRUE(sfu_.opens.empty());
    EXPECT_EQ(sfu_.answered_opens, 1);
    ASSERT_EQ(sfu_.joins.size(), 2U);
    EXPECT_EQ(sfu_.joins[0].user, "bob");
    sfu_.issue();
    sfu_.issue();
    EXPECT_EQ(outcomes(), std::vector(3, CallOutcome::Ticket));
    EXPECT_EQ(handler_->counters().opens, 1U);
    // Each ask read the member list: a removal is seen at the next ask.
    EXPECT_EQ(store_.reads, 3);
    EXPECT_EQ(handler_->rooms(), 1U);
}

TEST_F(CallHandlerTest, AMemberTakenOffTheListGetsNoTicketThoughTheRoomIsOpen) {
    ask("alice");
    sfu_.open();
    sfu_.issue();
    ASSERT_EQ(outcomes(), std::vector{CallOutcome::Ticket});
    // The list is read for every ask, on the owner: the removal counts at once.
    std::erase(store_.members_, "alice");
    ask("alice");
    EXPECT_EQ(outcomes(), (std::vector{CallOutcome::Ticket, CallOutcome::NotMember}));
    EXPECT_TRUE(sfu_.joins.empty());
    EXPECT_EQ(handler_->counters().tickets, 1U);
}

TEST_F(CallHandlerTest, AnAskTheStoreCouldNotTakeLeavesNoAskCounted) {
    handler_ = std::make_unique<chat::CallHandler>(store_, &sfu_, clock_,
                                                   chat::CallLimits{.max_in_flight = 1});
    store_.throwing = true;
    ask("alice");
    ask("alice");
    // Nothing answered them: the asker's own deadline does (rt::kOwnerAskTimeout).
    EXPECT_TRUE(answers_.empty());
    store_.throwing = false;
    ask("alice");
    ASSERT_EQ(sfu_.opens.size(), 1U) << "the failed asks were still counted in flight";
    sfu_.open();
    sfu_.issue();
    EXPECT_EQ(outcomes(), std::vector{CallOutcome::Ticket});
}

TEST_F(CallHandlerTest, AnOpenTheSfuCouldNotTakeAnswersItsWaitersAndLeavesTheRoomFree) {
    handler_ = std::make_unique<chat::CallHandler>(store_, &sfu_, clock_,
                                                   chat::CallLimits{.max_in_flight = 1});
    sfu_.throwing = true;
    ask("alice");
    EXPECT_EQ(errors_, std::vector{rt::RouteError::Unavailable});
    EXPECT_EQ(handler_->rooms(), 0U);
    // Neither the room nor the count is left behind: the next ask opens it.
    sfu_.throwing = false;
    ask("bob", kOtherDevice);
    ASSERT_EQ(sfu_.opens.size(), 1U);
    sfu_.open();
    sfu_.issue();
    EXPECT_EQ(outcomes(), std::vector{CallOutcome::Ticket});
}

TEST_F(CallHandlerTest, AsksWhileTheRoomIsOpeningWaitForThatOneOpen) {
    ask("alice");
    ask("bob", kOtherDevice);
    ask("alice", kOtherDevice);
    ASSERT_EQ(sfu_.opens.size(), 1U);
    sfu_.open();
    ASSERT_EQ(sfu_.joins.size(), 3U);
    for (int i = 0; i < 3; ++i) {
        sfu_.issue();
    }
    EXPECT_EQ(outcomes(), std::vector(3, CallOutcome::Ticket));
    EXPECT_EQ(sfu_.answered_opens, 1);
}

TEST_F(CallHandlerTest, AnSfuThatCannotBeReachedIsAnsweredAsRetryableAndTheNextAskOpensAgain) {
    ask("alice");
    ask("bob", kOtherDevice);
    sfu_.open(std::unexpected(MediaError::Unavailable));
    EXPECT_EQ(outcomes(), std::vector(2, CallOutcome::Unavailable));
    EXPECT_EQ(handler_->rooms(), 0U);
    ask("alice");
    ASSERT_EQ(sfu_.opens.size(), 1U);
    sfu_.open();
    // A join the SFU could not answer is retryable too; one it refused is not.
    sfu_.issue(std::unexpected(MediaError::Unavailable));
    ask("alice");
    sfu_.issue(std::unexpected(MediaError::Refused));
    EXPECT_EQ(outcomes(), (std::vector{CallOutcome::Unavailable, CallOutcome::Unavailable,
                                       CallOutcome::Unavailable, CallOutcome::Failed}));
    // Counted per SFU call: the failed open, then the failed join.
    EXPECT_EQ(handler_->counters().sfu_unavailable, 2U);
    EXPECT_EQ(handler_->counters().sfu_refused, 1U);
    // The handle survived the failed joins.
    EXPECT_EQ(handler_->rooms(), 1U);
}

TEST_F(CallHandlerTest, AMemberListThatCannotBeReadIsRetryable) {
    store_.down = true;
    ask("alice");
    EXPECT_EQ(outcomes(), std::vector{CallOutcome::Unavailable});
    EXPECT_TRUE(sfu_.opens.empty());
    EXPECT_EQ(handler_->counters().store_unavailable, 1U);
}

TEST_F(CallHandlerTest, WithoutAnSfuEveryAskIsAnsweredDisabled) {
    handler_ = std::make_unique<chat::CallHandler>(store_, nullptr, clock_, chat::CallLimits{});
    EXPECT_FALSE(handler_->enabled());
    ask("alice");
    EXPECT_EQ(outcomes(), std::vector{CallOutcome::Disabled});
    EXPECT_EQ(store_.reads, 0);
}

TEST_F(CallHandlerTest, AnAskThatDoesNotDecodeIsUnavailable) {
    const std::vector<std::byte> junk{std::byte{0x7F}, std::byte{0x01}};
    handler_->on_ask(room_id(), junk,
                     [this](std::expected<std::vector<std::byte>, rt::RouteError> r) noexcept {
                         errors_.push_back(r ? rt::RouteError::Conflict : r.error());
                     });
    EXPECT_EQ(errors_, std::vector{rt::RouteError::Unavailable});
}

TEST_F(CallHandlerTest, AHandleNobodyAskedForWithinTheIdleTimeIsLetGo) {
    ask("alice");
    sfu_.open();
    sfu_.issue();
    clock_.advance(chat::CallLimits{}.idle - core::Millis{1'000});
    handler_->sweep();
    EXPECT_EQ(handler_->rooms(), 1U);
    // Looked at once a second, however often it is called.
    clock_.advance(core::Millis{999});
    handler_->sweep();
    EXPECT_EQ(handler_->rooms(), 1U);
    clock_.advance(core::Millis{1});
    handler_->sweep();
    EXPECT_EQ(handler_->rooms(), 0U);
    // Let go, not closed: the call goes on, and the next ask opens the same generation.
    EXPECT_EQ(sfu_.closes, 0);
    ask("bob", kOtherDevice);
    ASSERT_EQ(sfu_.opens.size(), 1U);
    EXPECT_EQ(sfu_.opens[0].generation, chat::kCallGeneration);
}

class BoundedCallHandlerTest : public CallHandlerTest {
protected:
    BoundedCallHandlerTest()
        : CallHandlerTest(chat::CallLimits{.max_rooms = 1, .max_in_flight = 2}) {}
};

TEST_F(BoundedCallHandlerTest, AsksPastWhatTheNodeTakesAreBusy) {
    ask("alice");
    ask("bob", kOtherDevice);
    ask("alice", kOtherDevice);
    EXPECT_EQ(outcomes(), std::vector{CallOutcome::Busy});
    sfu_.open();
    sfu_.issue();
    sfu_.issue();
    EXPECT_EQ(outcomes(),
              (std::vector{CallOutcome::Busy, CallOutcome::Ticket, CallOutcome::Ticket}));
    EXPECT_EQ(handler_->counters().busy, 1U);
}

TEST_F(BoundedCallHandlerTest, ANewRoomPastTheCapWaitsForAnIdleHandleToGo) {
    ask("alice");
    sfu_.open();
    sfu_.issue();
    ask("alice", kDevice, kOtherRoom);
    EXPECT_EQ(outcomes(), (std::vector{CallOutcome::Ticket, CallOutcome::Busy}));
    EXPECT_TRUE(sfu_.opens.empty());
    // The first room's handle idles out; the cap makes room for the second at once, with no
    // sweep of the loop's own in between.
    clock_.advance(chat::CallLimits{}.idle);
    ask("alice", kDevice, kOtherRoom);
    ASSERT_EQ(sfu_.opens.size(), 1U);
    EXPECT_EQ(sfu_.opens[0].room, room_id(kOtherRoom));
    sfu_.open();
    sfu_.issue();
    EXPECT_EQ(outcomes(),
              (std::vector{CallOutcome::Ticket, CallOutcome::Busy, CallOutcome::Ticket}));
    EXPECT_EQ(handler_->rooms(), 1U);
}

TEST(CallCodec, RequestsAndAnswersComeBackAsTheyWereSent) {
    const chat::CallRequest request{.user = *core::UserId::parse("auth0|alice"),
                                    .device = *core::DeviceId::parse(kDevice)};
    const auto decoded = chat::decode_request(chat::encode_request(request));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->user, request.user);
    EXPECT_EQ(decoded->device, request.device);

    const chat::CallAnswer ticket{
        .outcome = CallOutcome::Ticket,
        .ticket = core::ports::MediaTicket{.endpoint = "wss://media.test",
                                           .credential = std::string(1000, 'j'),
                                           .expires_at = core::WallTime{core::Millis{1234567}}}};
    const auto back = chat::decode_answer(chat::encode_answer(ticket));
    ASSERT_TRUE(back && back->ticket);
    EXPECT_EQ(back->ticket->endpoint, "wss://media.test");
    EXPECT_EQ(back->ticket->credential, std::string(1000, 'j'));
    EXPECT_EQ(back->ticket->expires_at, core::WallTime{core::Millis{1234567}});
    const auto refused = chat::decode_answer(
        chat::encode_answer({.outcome = CallOutcome::NotMember, .ticket = std::nullopt}));
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->outcome, CallOutcome::NotMember);
    EXPECT_FALSE(refused->ticket);

    auto trailing = chat::encode_answer({.outcome = CallOutcome::Busy, .ticket = std::nullopt});
    trailing.push_back(std::byte{0});
    EXPECT_FALSE(chat::decode_answer(trailing));
    auto unknown = chat::encode_answer({.outcome = CallOutcome::Busy, .ticket = std::nullopt});
    unknown[1] = std::byte{7};
    EXPECT_FALSE(chat::decode_answer(unknown));
}

} // namespace

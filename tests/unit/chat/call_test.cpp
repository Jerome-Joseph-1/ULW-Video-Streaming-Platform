#include "call.hpp"
#include "presence_room.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
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
                 std::size_t limit,
                 core::ports::MessageCallback<std::vector<core::UserId>> done) override {
        ++lists;
        if (members_throwing) {
            throw std::bad_alloc();
        }
        if (list_down) {
            done(std::unexpected(core::ports::MessageStoreError::Unavailable));
            return;
        }
        std::vector<core::UserId> out;
        for (const std::string& m : members_) {
            if (out.size() < limit) {
                out.push_back(*core::UserId::parse(m));
            }
        }
        done(std::move(out));
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
    // The member list, as the ring reads it.
    bool list_down = false;
    bool members_throwing = false;
    int lists = 0;
};

// The room plane as the ring sees it: the notices, decoded.
class FakePlane final : public chat::IRingPlane {
public:
    void notify(const core::RoomId& room, std::span<const std::byte> notice) noexcept override {
        auto decoded = chat::decode_notice(notice);
        EXPECT_TRUE(decoded && chat::presence_room(decoded->to) == room);
        if (decoded) {
            sent.push_back(*decoded);
        }
    }
    [[nodiscard]] bool owns(const core::RoomId& /*room*/) const noexcept override { return true; }

    [[nodiscard]] std::vector<chat::RingEvent> events() const {
        std::vector<chat::RingEvent> out;
        out.reserve(sent.size());
        for (const chat::CallNotice& n : sent) {
            out.push_back(n.event);
        }
        return out;
    }

    std::vector<chat::CallNotice> sent;
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

    // Calls never ask: only the stream service's webhook path does (ADR-0093).
    void present(const core::RoomId& /*room*/, core::ports::MediaGeneration /*generation*/,
                 const core::UserId& /*user*/, const core::DeviceId& /*device*/,
                 core::ports::PresenceDone done) override {
        done(std::unexpected(MediaError::NotImplemented));
    }

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
    explicit CallHandlerTest(chat::CallLimits limits = {}) : handler_(make(limits)) {}

    std::unique_ptr<chat::CallHandler> make(chat::CallLimits limits = {}) {
        return std::make_unique<chat::CallHandler>(store_, &sfu_, plane_, clock_, random_, limits);
    }

    // Declines, cancels or ends `call` as `user`; the answer lands in answers_ as ask's do.
    void move(std::string_view user, chat::CallSignal signal, const chat::CallId& call,
              std::string_view room = kRoom) {
        const auto request = chat::encode_request(chat::CallSignalRequest{
            .user = *core::UserId::parse(user), .signal = signal, .call = call});
        handler_->on_ask(room_id(room), request,
                         [this](std::expected<std::vector<std::byte>, rt::RouteError> r) noexcept {
                             if (!r) {
                                 errors_.push_back(r.error());
                                 return;
                             }
                             answers_.push_back(*chat::decode_answer(*r));
                         });
    }

    // Asks as `user` from `device`; the answer lands in answers_, in the order they come.
    void ask(std::string_view user = "alice", std::string_view device = kDevice,
             std::string_view room = kRoom) {
        const auto request = chat::encode_request(chat::CallRequest{
            .user = *core::UserId::parse(user), .device = *core::DeviceId::parse(device)});
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
    FakePlane plane_;
    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
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
    handler_ = make(chat::CallLimits{.max_in_flight = 1});
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
    handler_ = make(chat::CallLimits{.max_in_flight = 1});
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
    handler_ = std::make_unique<chat::CallHandler>(store_, nullptr, plane_, clock_, random_,
                                                   chat::CallLimits{});
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

TEST_F(CallHandlerTest, TheFirstTicketRingsTheOtherMemberAndTheCalleesTicketAnswersIt) {
    ask("alice");
    // The list is read before the SFU is asked: the ticket may start the call.
    EXPECT_EQ(store_.lists, 1);
    sfu_.open();
    EXPECT_TRUE(plane_.sent.empty()) << "rung before the caller had a ticket";
    sfu_.issue();
    ASSERT_EQ(outcomes(), std::vector{CallOutcome::Ticket});
    ASSERT_TRUE(answers_[0].call);
    const chat::CallId call = *answers_[0].call;
    EXPECT_EQ(plane_.events(), std::vector(2, chat::RingEvent::Ringing));
    EXPECT_EQ(handler_->calls(), 1U);

    ask("bob", kOtherDevice);
    // A call rings already: no list is read for an answer.
    EXPECT_EQ(store_.lists, 1);
    sfu_.issue();
    ASSERT_EQ(answers_.size(), 2U);
    EXPECT_EQ(answers_[1].call, call);
    EXPECT_EQ(plane_.events(), (std::vector{chat::RingEvent::Ringing, chat::RingEvent::Ringing,
                                            chat::RingEvent::Answered, chat::RingEvent::Answered}));
    EXPECT_EQ(handler_->ring_counters().answered, 1U);
}

TEST_F(CallHandlerTest, ATicketTheSfuRefusedRingsNobody) {
    ask("alice");
    sfu_.open();
    sfu_.issue(std::unexpected(MediaError::Unavailable));
    EXPECT_EQ(outcomes(), std::vector{CallOutcome::Unavailable});
    EXPECT_TRUE(plane_.sent.empty());
    EXPECT_EQ(handler_->calls(), 0U);
}

TEST_F(CallHandlerTest, TheCalleeDeclinesOnTheOwnerWhichChecksTheListAgain) {
    ask("alice");
    sfu_.open();
    sfu_.issue();
    const chat::CallId call = *answers_.at(0).call;
    plane_.sent.clear();
    const int reads = store_.reads;
    move("bob", chat::CallSignal::Decline, call);
    EXPECT_EQ(store_.reads, reads + 1);
    ASSERT_EQ(answers_.size(), 2U);
    EXPECT_EQ(answers_[1].outcome, CallOutcome::Done);
    EXPECT_EQ(answers_[1].caller, core::UserId::parse("alice").value());
    EXPECT_EQ(plane_.events(), std::vector(2, chat::RingEvent::Declined));
    EXPECT_EQ(handler_->calls(), 0U);
    // Nothing left to decline, or to cancel.
    move("bob", chat::CallSignal::Decline, call);
    move("alice", chat::CallSignal::Cancel, call);
    EXPECT_EQ(outcomes(), (std::vector{CallOutcome::Ticket, CallOutcome::Done, CallOutcome::NoCall,
                                       CallOutcome::NoCall}));
    EXPECT_EQ(handler_->counters().no_call, 2U);
}

TEST_F(CallHandlerTest, OnlyAMemberOfADirectChatMovesItsCall) {
    ask("alice");
    sfu_.open();
    sfu_.issue();
    const chat::CallId call = *answers_.at(0).call;
    plane_.sent.clear();
    move("mallory", chat::CallSignal::Decline, call);
    // Removed from the list since: the owner's read says so, whatever bob's join said.
    std::erase(store_.members_, "bob");
    move("bob", chat::CallSignal::Decline, call);
    store_.members_.emplace_back("bob");
    store_.kind = RoomKind::GroupChat;
    move("bob", chat::CallSignal::Decline, call);
    store_.kind = RoomKind::DirectChat;
    store_.down = true;
    move("bob", chat::CallSignal::Decline, call);
    EXPECT_EQ(outcomes(),
              (std::vector{CallOutcome::Ticket, CallOutcome::NotMember, CallOutcome::NotMember,
                           CallOutcome::NotCallable, CallOutcome::Unavailable}));
    EXPECT_TRUE(plane_.sent.empty());
    EXPECT_EQ(handler_->calls(), 1U);
    // A store that cannot take the read leaves nothing counted.
    store_.down = false;
    store_.throwing = true;
    move("bob", chat::CallSignal::Decline, call);
    store_.throwing = false;
    move("bob", chat::CallSignal::Decline, call);
    EXPECT_EQ(answers_.back().outcome, CallOutcome::Done);
}

TEST_F(CallHandlerTest, AMemberListTheRingCannotReadIsRetryableAndTheSfuIsNotAsked) {
    store_.list_down = true;
    ask("alice");
    EXPECT_EQ(outcomes(), std::vector{CallOutcome::Unavailable});
    EXPECT_TRUE(sfu_.opens.empty());
    EXPECT_EQ(handler_->counters().store_unavailable, 1U);
    store_.list_down = false;
    store_.members_throwing = true;
    ask("alice");
    // Nothing answered it: the asker's own deadline does (rt::kOwnerAskTimeout).
    EXPECT_EQ(answers_.size(), 1U);
    EXPECT_TRUE(errors_.empty());
    store_.members_throwing = false;
    // Neither left an ask counted.
    handler_ = make(chat::CallLimits{.max_in_flight = 1});
    ask("alice");
    EXPECT_EQ(sfu_.opens.size(), 1U);
}

TEST_F(CallHandlerTest, TheRingRunsOutOnTheSweepOfTheLoop) {
    ask("alice");
    sfu_.open();
    sfu_.issue();
    plane_.sent.clear();
    clock_.advance(chat::RingLimits{}.ring_timeout);
    handler_->sweep();
    EXPECT_EQ(plane_.events().back(), chat::RingEvent::Missed);
    EXPECT_EQ(handler_->calls(), 0U);
    EXPECT_EQ(handler_->ring_counters().missed, 1U);
}

TEST_F(CallHandlerTest, ATicketForAnAnswerThatCameAfterTheRingEndedRingsNobody) {
    ask("alice");
    sfu_.open();
    sfu_.issue();
    const chat::CallId call = *answers_.at(0).call;
    // Bob answers while alice cancels: the cancel wins the race to the owner.
    ask("bob", kOtherDevice);
    move("alice", chat::CallSignal::Cancel, call);
    plane_.sent.clear();
    sfu_.issue();
    ASSERT_EQ(answers_.size(), 3U);
    EXPECT_EQ(answers_[2].outcome, CallOutcome::Ticket);
    EXPECT_FALSE(answers_[2].call);
    EXPECT_TRUE(plane_.sent.empty());
}

TEST_F(CallHandlerTest, SignalsWithoutAnSfuAreDisabledAsTicketsAre) {
    handler_ = std::make_unique<chat::CallHandler>(store_, nullptr, plane_, clock_, random_,
                                                   chat::CallLimits{});
    move("bob", chat::CallSignal::Decline, chat::CallId::generate(clock_, random_));
    EXPECT_EQ(outcomes(), std::vector{CallOutcome::Disabled});
}

TEST_F(CallHandlerTest, ATicketThatWouldRingTooSoonIsRefusedBeforeTheSfuWithWhenToRetry) {
    ask("alice");
    sfu_.open();
    sfu_.issue();
    const chat::CallId call = *answers_.at(0).call;
    move("bob", chat::CallSignal::Decline, call);
    // Alice rings again at once: held by bob's decline, without a list read or an SFU call.
    const int lists = store_.lists;
    ask("alice");
    ASSERT_EQ(answers_.size(), 3U);
    EXPECT_EQ(answers_[2].outcome, CallOutcome::RingLimited);
    EXPECT_EQ(answers_[2].retry_after, chat::RingLimits{}.decline_cooldown);
    EXPECT_EQ(store_.lists, lists);
    EXPECT_TRUE(sfu_.joins.empty());
    EXPECT_EQ(handler_->ring_counters().limited, 1U);
    // Bob may call back.
    ask("bob", kOtherDevice);
    sfu_.issue();
    EXPECT_EQ(answers_.back().outcome, CallOutcome::Ticket);
    EXPECT_TRUE(answers_.back().call);
}

TEST_F(CallHandlerTest, ARingLimitReachedWhileTheTicketWasIssuedIsRefusedThen) {
    handler_ = make(chat::CallLimits{.ring = chat::RingLimits{.rings_per_window = 1}});
    // Two asks of an idle room at once: both pass the check; the second ticket would ring again.
    ask("alice");
    ask("alice", kOtherDevice);
    sfu_.open();
    sfu_.issue();
    const chat::CallId call = *answers_.at(0).call;
    move("alice", chat::CallSignal::Cancel, call);
    sfu_.issue();
    ASSERT_EQ(answers_.size(), 3U);
    EXPECT_EQ(answers_[2].outcome, CallOutcome::RingLimited);
    EXPECT_EQ(answers_[2].retry_after, chat::RingLimits{}.ring_window);
}

TEST_F(CallHandlerTest, ACalleesTicketAskedJustBeforeTheTimeoutAnswersOnceTheSfuIssuesIt) {
    ask("alice");
    sfu_.open();
    sfu_.issue();
    plane_.sent.clear();
    clock_.advance(chat::RingLimits{}.ring_timeout - core::Millis{1});
    handler_->sweep();
    plane_.sent.clear();
    ask("bob", kOtherDevice);
    // The SFU is slow; the timeout passes meanwhile.
    clock_.advance(core::Millis{3'000});
    handler_->sweep();
    EXPECT_TRUE(plane_.sent.empty());
    sfu_.issue();
    EXPECT_EQ(answers_.back().outcome, CallOutcome::Ticket);
    EXPECT_EQ(plane_.events(), std::vector(2, chat::RingEvent::Answered));
    EXPECT_EQ(handler_->ring_counters().missed, 0U);
}

class RingBoundedCallHandlerTest : public CallHandlerTest {
protected:
    RingBoundedCallHandlerTest()
        : CallHandlerTest(chat::CallLimits{.ring = chat::RingLimits{.max_calls = 1}}) {}
};

TEST_F(RingBoundedCallHandlerTest, ATicketThatWouldStartACallPastTheCapIsBusyBeforeTheSfu) {
    ask("alice");
    sfu_.open();
    sfu_.issue();
    ask("alice", kDevice, kOtherRoom);
    EXPECT_EQ(outcomes(), (std::vector{CallOutcome::Ticket, CallOutcome::Busy}));
    EXPECT_TRUE(sfu_.opens.empty());
    // Two idle rooms asked at once both pass the check; the second to get its ticket is busy.
    handler_ = make(chat::CallLimits{.ring = chat::RingLimits{.max_calls = 1}});
    answers_.clear();
    ask("alice", kDevice, kOtherRoom);
    ask("alice", kDevice, kRoom);
    sfu_.open();
    sfu_.open();
    sfu_.issue();
    sfu_.issue();
    EXPECT_EQ(outcomes(), (std::vector{CallOutcome::Ticket, CallOutcome::Busy}));
    EXPECT_EQ(handler_->calls(), 1U);
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
    const auto* ticket_ask = std::get_if<chat::CallRequest>(&*decoded);
    ASSERT_NE(ticket_ask, nullptr);
    EXPECT_EQ(ticket_ask->user, request.user);
    EXPECT_EQ(ticket_ask->device, request.device);

    const ulw::test::FakeClock clock;
    ulw::test::FakeRandom random;
    const chat::CallId call = chat::CallId::generate(clock, random);
    for (const chat::CallSignal signal :
         {chat::CallSignal::Decline, chat::CallSignal::Cancel, chat::CallSignal::End}) {
        const chat::CallSignalRequest move{.user = request.user, .signal = signal, .call = call};
        const auto back = chat::decode_request(chat::encode_request(move));
        ASSERT_TRUE(back);
        const auto* moved = std::get_if<chat::CallSignalRequest>(&*back);
        ASSERT_NE(moved, nullptr);
        EXPECT_EQ(moved->user, move.user);
        EXPECT_EQ(moved->signal, signal);
        EXPECT_EQ(moved->call, call);
    }
    auto unknown_ask = chat::encode_request(request);
    unknown_ask[1] = std::byte{4};
    EXPECT_FALSE(chat::decode_request(unknown_ask));
    auto short_ask = chat::encode_request(request);
    short_ask.pop_back();
    EXPECT_FALSE(chat::decode_request(short_ask));
    auto long_move = chat::encode_request(chat::CallSignalRequest{
        .user = request.user, .signal = chat::CallSignal::End, .call = call});
    long_move.push_back(std::byte{'x'});
    EXPECT_FALSE(chat::decode_request(long_move));

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
    unknown[1] = std::byte{10};
    EXPECT_FALSE(chat::decode_answer(unknown));

    // A ticket names its call; a signal done names the call's caller.
    chat::CallAnswer rung = ticket;
    rung.call = call;
    const auto with_call = chat::decode_answer(chat::encode_answer(rung));
    ASSERT_TRUE(with_call);
    EXPECT_EQ(with_call->call, call);
    const auto done = chat::decode_answer(chat::encode_answer({.outcome = CallOutcome::Done,
                                                               .ticket = std::nullopt,
                                                               .call = call,
                                                               .caller = request.user}));
    ASSERT_TRUE(done);
    EXPECT_EQ(done->caller, request.user);
    auto truncated = chat::encode_answer(rung);
    truncated.pop_back();
    EXPECT_FALSE(chat::decode_answer(truncated));
    EXPECT_FALSE(chat::decode_answer(
        chat::encode_answer({.outcome = CallOutcome::Done, .ticket = std::nullopt})));

    // A refused ring says when to ask again.
    const auto limited =
        chat::decode_answer(chat::encode_answer({.outcome = CallOutcome::RingLimited,
                                                 .ticket = std::nullopt,
                                                 .retry_after = core::Millis{29'500}}));
    ASSERT_TRUE(limited);
    EXPECT_EQ(limited->retry_after, core::Millis{29'500});
    auto cut = chat::encode_answer({.outcome = CallOutcome::RingLimited,
                                    .ticket = std::nullopt,
                                    .retry_after = core::Millis{1}});
    cut.pop_back();
    EXPECT_FALSE(chat::decode_answer(cut));

    // Layout 1, #132's, is another node's: neither its asks nor its answers are read.
    auto old_ask = chat::encode_request(request);
    old_ask[0] = std::byte{1};
    EXPECT_FALSE(chat::decode_request(old_ask));
    auto old_answer = chat::encode_answer(rung);
    old_answer[0] = std::byte{1};
    EXPECT_FALSE(chat::decode_answer(old_answer));
    auto old_refusal =
        chat::encode_answer({.outcome = CallOutcome::NotMember, .ticket = std::nullopt});
    old_refusal[0] = std::byte{1};
    EXPECT_FALSE(chat::decode_answer(old_refusal));
}

} // namespace

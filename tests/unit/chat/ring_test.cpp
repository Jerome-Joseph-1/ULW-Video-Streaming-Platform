#include "presence_room.hpp"
#include "ring.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

namespace {

using chat::CallId;
using chat::CallSignal;
using chat::RingEvent;

constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
constexpr std::string_view kOtherRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fe";

core::RoomId room_id(std::string_view room = kRoom) {
    return *core::RoomId::parse(room);
}

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

// The room plane as the ring sees it: every notice it was handed, decoded, and whether this node
// owns the rooms.
class FakePlane final : public chat::IRingPlane {
public:
    void notify(const core::RoomId& room, std::span<const std::byte> notice) noexcept override {
        auto decoded = chat::decode_notice(notice);
        // Every notice goes to its member's presence room.
        EXPECT_TRUE(decoded && chat::presence_room(decoded->to) == room);
        if (decoded) {
            sent.push_back(*decoded);
        }
    }
    [[nodiscard]] bool owns(const core::RoomId& /*room*/) const noexcept override { return owning; }

    // What was sent since the last call, as "event:to[:by]".
    std::vector<std::string> take() {
        std::vector<std::string> out;
        for (const chat::CallNotice& n : sent) {
            std::string line =
                std::to_string(static_cast<int>(n.event)) + ":" + std::string(n.to.view());
            if (n.by) {
                line += ":" + std::string(n.by->view());
            }
            out.push_back(line);
        }
        sent.clear();
        return out;
    }

    std::vector<chat::CallNotice> sent;
    bool owning = true;
};

const std::vector<core::UserId> kPair{user("alice"), user("bob")};

std::string line(RingEvent event, std::string_view to, std::string_view by = {}) {
    std::string out = std::to_string(static_cast<int>(event)) + ":" + std::string(to);
    if (!by.empty()) {
        out += ":" + std::string(by);
    }
    return out;
}

class RingerTest : public ::testing::Test {
protected:
    explicit RingerTest(chat::RingLimits limits = {}) : ringer_(plane_, clock_, random_, limits) {}

    // alice calls bob in kRoom; the call's id.
    CallId ring() {
        const auto call = ringer_.ticketed(room_id(), user("alice"), kPair);
        EXPECT_TRUE(call && *call);
        return call.value_or(std::nullopt).value_or(CallId::generate(clock_, random_));
    }

    FakePlane plane_;
    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
    chat::Ringer ringer_;
};

TEST_F(RingerTest, TheFirstTicketRingsEveryOtherMemberAndTellsTheCallersOwnDevices) {
    EXPECT_TRUE(ringer_.idle(room_id()));
    const CallId call = ring();
    EXPECT_FALSE(ringer_.idle(room_id()));
    ASSERT_EQ(plane_.sent.size(), 2U);
    for (const chat::CallNotice& n : plane_.sent) {
        EXPECT_EQ(n.event, RingEvent::Ringing);
        EXPECT_EQ(n.room, room_id());
        EXPECT_EQ(n.call, call);
        EXPECT_EQ(n.from, user("alice"));
        EXPECT_FALSE(n.by);
        EXPECT_EQ(n.expires_at, clock_.wall_now() + chat::RingLimits{}.ring_timeout);
    }
    EXPECT_EQ(plane_.take(),
              (std::vector{line(RingEvent::Ringing, "alice"), line(RingEvent::Ringing, "bob")}));
    EXPECT_EQ(ringer_.calls(), 1U);
    EXPECT_EQ(ringer_.counters().started, 1U);
    EXPECT_EQ(ringer_.counters().notices, 2U);

    // The caller asking again (a second device, an app restarted) rings nobody anew.
    EXPECT_EQ(ringer_.ticketed(room_id(), user("alice"), std::nullopt), call);
    EXPECT_EQ(ringer_.ticketed(room_id(), user("alice"), kPair), call);
    EXPECT_TRUE(plane_.take().empty());
}

TEST_F(RingerTest, TheCalleesTicketAnswersAndEveryDeviceOfBothHearsItOnce) {
    const CallId call = ring();
    plane_.take();
    EXPECT_EQ(ringer_.ticketed(room_id(), user("bob"), std::nullopt), call);
    EXPECT_EQ(plane_.take(), (std::vector{line(RingEvent::Answered, "alice", "bob"),
                                          line(RingEvent::Answered, "bob", "bob")}));
    // Bob's second device, and alice coming back, join the answered call: nothing rings.
    EXPECT_EQ(ringer_.ticketed(room_id(), user("bob"), std::nullopt), call);
    EXPECT_EQ(ringer_.ticketed(room_id(), user("alice"), kPair), call);
    EXPECT_TRUE(plane_.take().empty());
    EXPECT_EQ(ringer_.counters().answered, 1U);
    // An answered call is neither declined nor cancelled.
    EXPECT_FALSE(ringer_.signal(room_id(), user("bob"), CallSignal::Decline, call));
    EXPECT_FALSE(ringer_.signal(room_id(), user("alice"), CallSignal::Cancel, call));
}

TEST_F(RingerTest, ADeclineEndsTheRingForBothAndOnlyTheCalleeMayDecline) {
    const CallId call = ring();
    plane_.take();
    const CallId other = CallId::generate(clock_, random_);
    EXPECT_FALSE(ringer_.signal(room_id(), user("alice"), CallSignal::Decline, call));
    EXPECT_FALSE(ringer_.signal(room_id(), user("bob"), CallSignal::Decline, other));
    EXPECT_FALSE(ringer_.signal(room_id(kOtherRoom), user("bob"), CallSignal::Decline, call));
    EXPECT_FALSE(ringer_.signal(room_id(), user("bob"), CallSignal::End, call));
    EXPECT_TRUE(plane_.take().empty());

    EXPECT_EQ(ringer_.signal(room_id(), user("bob"), CallSignal::Decline, call), user("alice"));
    EXPECT_EQ(plane_.take(), (std::vector{line(RingEvent::Declined, "alice", "bob"),
                                          line(RingEvent::Declined, "bob", "bob")}));
    EXPECT_TRUE(ringer_.idle(room_id()));
    EXPECT_EQ(ringer_.counters().declined, 1U);
    // Once.
    EXPECT_FALSE(ringer_.signal(room_id(), user("bob"), CallSignal::Decline, call));
    // A ticket asked while the call rang, answered after it ended, rings nobody.
    EXPECT_EQ(ringer_.ticketed(room_id(), user("bob"), std::nullopt), std::nullopt);
    EXPECT_TRUE(plane_.take().empty());
}

TEST_F(RingerTest, OnlyTheCallerCancelsAndTheCalleesStopRinging) {
    const CallId call = ring();
    plane_.take();
    EXPECT_FALSE(ringer_.signal(room_id(), user("bob"), CallSignal::Cancel, call));
    EXPECT_EQ(ringer_.signal(room_id(), user("alice"), CallSignal::Cancel, call), user("alice"));
    EXPECT_EQ(plane_.take(), (std::vector{line(RingEvent::Cancelled, "alice", "alice"),
                                          line(RingEvent::Cancelled, "bob", "alice")}));
    EXPECT_EQ(ringer_.counters().cancelled, 1U);
    EXPECT_EQ(ringer_.calls(), 0U);
}

TEST_F(RingerTest, ARingNobodyAnswersIsAnnouncedAgainThenMissedForBothAtTheTimeout) {
    const CallId call = ring();
    plane_.take();
    const chat::RingLimits limits;
    clock_.advance(limits.announce_every - core::Millis{1});
    ringer_.tick();
    EXPECT_TRUE(plane_.take().empty());
    clock_.advance(core::Millis{1});
    ringer_.tick();
    // Again, for the sockets that connected since and any notice lost on the way; same call,
    // same deadline.
    ASSERT_EQ(plane_.sent.size(), 2U);
    EXPECT_EQ(plane_.sent[0].call, call);
    EXPECT_EQ(plane_.sent[0].expires_at,
              clock_.wall_now() - limits.announce_every + limits.ring_timeout);
    EXPECT_EQ(plane_.take(),
              (std::vector{line(RingEvent::Ringing, "alice"), line(RingEvent::Ringing, "bob")}));
    clock_.advance(limits.announce_every);
    ringer_.tick();
    EXPECT_EQ(plane_.take().size(), 2U);
    clock_.advance(limits.ring_timeout - (2 * limits.announce_every) - core::Millis{1});
    ringer_.tick();
    EXPECT_TRUE(plane_.take().empty());
    clock_.advance(core::Millis{1});
    ringer_.tick();
    EXPECT_EQ(plane_.take(),
              (std::vector{line(RingEvent::Missed, "alice"), line(RingEvent::Missed, "bob")}));
    EXPECT_TRUE(ringer_.idle(room_id()));
    EXPECT_EQ(ringer_.counters().missed, 1U);
    // A late answer finds nothing to answer; a late decline nothing to decline.
    EXPECT_FALSE(ringer_.signal(room_id(), user("bob"), CallSignal::Decline, call));
    EXPECT_EQ(ringer_.ticketed(room_id(), user("bob"), std::nullopt), std::nullopt);
    // Calling back is a new call, from bob.
    const auto back = ringer_.ticketed(room_id(), user("bob"), kPair);
    ASSERT_TRUE(back && *back);
    EXPECT_NE(**back, call);
    EXPECT_EQ(plane_.sent.at(0).from, user("bob"));
}

TEST_F(RingerTest, ALateTickRingsOutAtOnceWithoutAnnouncingWhatItMissed) {
    ring();
    plane_.take();
    clock_.advance(chat::RingLimits{}.ring_timeout * 3);
    ringer_.tick();
    EXPECT_EQ(plane_.take(),
              (std::vector{line(RingEvent::Missed, "alice"), line(RingEvent::Missed, "bob")}));
}

TEST_F(RingerTest, EitherMemberEndsAnAnsweredCallWhichIsOtherwiseHeldWhileTicketsAreAsked) {
    const CallId call = ring();
    ASSERT_TRUE(ringer_.ticketed(room_id(), user("bob"), std::nullopt));
    plane_.take();
    const chat::RingLimits limits;
    // Asking again keeps it: a member coming back joins without ringing the other.
    clock_.advance(limits.answered_hold - core::Millis{1});
    EXPECT_EQ(ringer_.ticketed(room_id(), user("alice"), std::nullopt), call);
    clock_.advance(limits.answered_hold - core::Millis{1});
    ringer_.tick();
    EXPECT_FALSE(ringer_.idle(room_id()));
    EXPECT_FALSE(ringer_.signal(room_id(), user("mallory"), CallSignal::End, call));
    EXPECT_EQ(ringer_.signal(room_id(), user("bob"), CallSignal::End, call), user("alice"));
    EXPECT_EQ(plane_.take(), (std::vector{line(RingEvent::Ended, "alice", "bob"),
                                          line(RingEvent::Ended, "bob", "bob")}));
    EXPECT_EQ(ringer_.counters().ended, 1U);
    EXPECT_TRUE(ringer_.idle(room_id()));
}

TEST_F(RingerTest, AnAnsweredCallNobodyAsksAboutIsForgottenQuietlyAfterItsHold) {
    ring();
    ASSERT_TRUE(ringer_.ticketed(room_id(), user("bob"), std::nullopt));
    plane_.take();
    clock_.advance(chat::RingLimits{}.answered_hold);
    ringer_.tick();
    EXPECT_TRUE(ringer_.idle(room_id()));
    EXPECT_TRUE(plane_.take().empty());
    // The next ticket rings again.
    ASSERT_TRUE(ringer_.ticketed(room_id(), user("alice"), kPair).value_or(std::nullopt));
    EXPECT_EQ(plane_.take().size(), 2U);
}

TEST_F(RingerTest, NobodyElseOnTheListMeansNothingRings) {
    EXPECT_EQ(ringer_.ticketed(room_id(), user("alice"), std::vector{user("alice")}), std::nullopt);
    EXPECT_EQ(ringer_.ticketed(room_id(), user("alice"), std::vector<core::UserId>{}),
              std::nullopt);
    EXPECT_TRUE(plane_.sent.empty());
    EXPECT_TRUE(ringer_.idle(room_id()));
}

TEST_F(RingerTest, AListThatGrewPastTwoRingsEachOtherMemberOnceAndAnyOfThemAnswers) {
    const std::vector<core::UserId> listed{user("alice"), user("bob"), user("carol"), user("bob")};
    const auto call = ringer_.ticketed(room_id(), user("alice"), listed);
    ASSERT_TRUE(call && *call);
    EXPECT_EQ(plane_.take().size(), 3U);
    EXPECT_EQ(ringer_.ticketed(room_id(), user("carol"), std::nullopt), **call);
    EXPECT_EQ(plane_.take().size(), 3U);
}

TEST_F(RingerTest, ADeposedOwnerForgetsItsCallsWithoutAWordAndANewOneKnowsNoneOfThem) {
    const CallId call = ring();
    plane_.take();
    plane_.owning = false;
    clock_.advance(chat::RingLimits{}.announce_every);
    ringer_.tick();
    EXPECT_TRUE(plane_.take().empty());
    EXPECT_TRUE(ringer_.idle(room_id()));
    EXPECT_EQ(ringer_.counters().orphaned, 1U);

    // The node that took the room over (or this one, restarted) starts with no calls: a decline
    // finds none, and the callee's ticket, read with the list, rings the caller, whose client
    // answers it as calls.md says.
    FakePlane fresh_plane;
    chat::Ringer fresh(fresh_plane, clock_, random_, {});
    EXPECT_FALSE(fresh.signal(room_id(), user("bob"), CallSignal::Decline, call));
    const auto back = fresh.ticketed(room_id(), user("bob"), kPair);
    ASSERT_TRUE(back && *back);
    EXPECT_EQ(fresh_plane.take(),
              (std::vector{line(RingEvent::Ringing, "bob"), line(RingEvent::Ringing, "alice")}));
}

class BoundedRingerTest : public RingerTest {
protected:
    BoundedRingerTest() : RingerTest(chat::RingLimits{.max_calls = 1}) {}
};

TEST_F(BoundedRingerTest, ACallPastTheCapIsBusyAndRingsNobody) {
    ring();
    plane_.take();
    EXPECT_TRUE(ringer_.full());
    const auto refused = ringer_.ticketed(room_id(kOtherRoom), user("alice"), kPair);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().why, chat::RingRefusal::Why::Busy);
    EXPECT_TRUE(plane_.take().empty());
    EXPECT_EQ(ringer_.counters().busy, 1U);
    // The call already ringing is not affected.
    EXPECT_TRUE(ringer_.ticketed(room_id(), user("bob"), std::nullopt).value_or(std::nullopt));
}

class RateLimitedRingerTest : public RingerTest {
protected:
    RateLimitedRingerTest()
        : RingerTest(chat::RingLimits{.rings_per_window = 2, .ring_window = core::Millis{60'000}}) {
    }
};

TEST_F(RateLimitedRingerTest, ARoomRingsAtMostSoOftenAndIsToldWhenItMayAgain) {
    // A loop of ticket and cancel, two seconds a round.
    const CallId first = ring();
    ASSERT_TRUE(ringer_.signal(room_id(), user("alice"), CallSignal::Cancel, first));
    clock_.advance(core::Millis{2'000});
    const CallId second = ring();
    ASSERT_TRUE(ringer_.signal(room_id(), user("alice"), CallSignal::Cancel, second));
    clock_.advance(core::Millis{2'000});
    plane_.take();
    const auto refused = ringer_.ticketed(room_id(), user("alice"), kPair);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().why, chat::RingRefusal::Why::Limited);
    // The first ring leaves the window 60 s after it started.
    EXPECT_EQ(refused.error().retry_after, core::Millis{56'000});
    EXPECT_TRUE(plane_.take().empty());
    // Per room, whoever calls: bob is held to it too; another room is not.
    EXPECT_EQ(ringer_.ring_limited(room_id(), user("bob")), core::Millis{56'000});
    EXPECT_TRUE(ringer_.ticketed(room_id(kOtherRoom), user("alice"), kPair).value_or(std::nullopt));
    EXPECT_EQ(ringer_.counters().limited, 2U);
    clock_.advance(core::Millis{56'000});
    ringer_.tick();
    EXPECT_TRUE(ringer_.ticketed(room_id(), user("alice"), kPair).value_or(std::nullopt));
}

TEST_F(RingerTest, ADeclinedCallerWaitsBeforeRingingAgainWhileTheCalleeMayCallBack) {
    const CallId call = ring();
    ASSERT_TRUE(ringer_.signal(room_id(), user("bob"), CallSignal::Decline, call));
    plane_.take();
    const chat::RingLimits limits;
    const auto refused = ringer_.ticketed(room_id(), user("alice"), kPair);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().why, chat::RingRefusal::Why::Limited);
    EXPECT_EQ(refused.error().retry_after, limits.decline_cooldown);
    EXPECT_TRUE(plane_.take().empty());
    clock_.advance(limits.decline_cooldown - core::Millis{1});
    EXPECT_EQ(ringer_.ring_limited(room_id(), user("alice")), core::Millis{1});
    // Bob calling back is not held by his own decline.
    EXPECT_FALSE(ringer_.ring_limited(room_id(), user("bob")));
    clock_.advance(core::Millis{1});
    EXPECT_FALSE(ringer_.ring_limited(room_id(), user("alice")));
}

TEST_F(RingerTest, ACalleeAnsweringJustBeforeTheTimeoutIsNotLostToMissed) {
    const CallId call = ring();
    plane_.take();
    const chat::RingLimits limits;
    clock_.advance(limits.ring_timeout - core::Millis{1});
    ringer_.tick();
    plane_.take();
    // Bob's ticket ask reaches the owner a millisecond before the timeout; the SFU takes its
    // time issuing it.
    ringer_.answering(room_id(), user("bob"));
    clock_.advance(core::Millis{1'000});
    ringer_.tick();
    EXPECT_TRUE(plane_.take().empty()) << "rung out while bob's ticket was being issued";
    EXPECT_EQ(ringer_.counters().graced, 1U);
    EXPECT_EQ(ringer_.ticketed(room_id(), user("bob"), std::nullopt), call);
    EXPECT_EQ(plane_.take(), (std::vector{line(RingEvent::Answered, "alice", "bob"),
                                          line(RingEvent::Answered, "bob", "bob")}));
    EXPECT_EQ(ringer_.counters().missed, 0U);
}

TEST_F(RingerTest, AnAnswerThatNeverComesRingsOutAtTheEndOfTheGraceAndALateOneHoldsNothing) {
    ring();
    plane_.take();
    const chat::RingLimits limits;
    // The caller, and an ask after the timeout, hold nothing.
    ringer_.answering(room_id(), user("alice"));
    clock_.advance(limits.ring_timeout - core::Millis{1});
    ringer_.answering(room_id(), user("bob"));
    clock_.advance(core::Millis{1});
    ringer_.tick();
    EXPECT_TRUE(plane_.take().empty());
    clock_.advance(limits.answer_grace - core::Millis{1});
    ringer_.answering(room_id(), user("bob"));
    ringer_.tick();
    EXPECT_TRUE(plane_.take().empty());
    clock_.advance(core::Millis{1});
    ringer_.tick();
    EXPECT_EQ(plane_.take(),
              (std::vector{line(RingEvent::Missed, "alice"), line(RingEvent::Missed, "bob")}));
    // Nothing to answer once rung out.
    ringer_.answering(room_id(), user("bob"));
    EXPECT_TRUE(ringer_.idle(room_id()));
}

TEST_F(RingerTest, AnswerAskedAfterTheTimeoutDoesNotHoldTheRing) {
    ring();
    plane_.take();
    const chat::RingLimits limits;
    clock_.advance(limits.ring_timeout);
    ringer_.answering(room_id(), user("bob"));
    ringer_.tick();
    EXPECT_EQ(plane_.take().size(), 2U);
    EXPECT_EQ(ringer_.counters().graced, 0U);
}

// Bob's ticket ask has reached the owner a millisecond before the timeout, and the timeout has
// passed while the SFU issues it: the call is held in its answer grace.
class GracedRingerTest : public RingerTest {
protected:
    CallId held() {
        const CallId call = ring();
        clock_.advance(chat::RingLimits{}.ring_timeout - core::Millis{1});
        ringer_.answering(room_id(), user("bob"));
        clock_.advance(core::Millis{1'000});
        ringer_.tick();
        plane_.take();
        EXPECT_EQ(ringer_.counters().graced, 1U);
        EXPECT_FALSE(ringer_.idle(room_id()));
        return call;
    }
};

TEST_F(GracedRingerTest, ADeclineDuringTheGraceEndsTheCallAndTheTicketThatFollowsRingsNobody) {
    const CallId call = held();
    // Bob's other device declines while his first device's ticket is being issued.
    EXPECT_EQ(ringer_.signal(room_id(), user("bob"), CallSignal::Decline, call), user("alice"));
    EXPECT_EQ(plane_.take(), (std::vector{line(RingEvent::Declined, "alice", "bob"),
                                          line(RingEvent::Declined, "bob", "bob")}));
    EXPECT_EQ(ringer_.ticketed(room_id(), user("bob"), std::nullopt), std::nullopt);
    clock_.advance(chat::RingLimits{}.answer_grace);
    ringer_.tick();
    EXPECT_TRUE(plane_.take().empty()) << "rang out a call already declined";
    EXPECT_EQ(ringer_.counters().missed, 0U);
}

TEST_F(GracedRingerTest, ACancelDuringTheGraceWinsOverTheAnswerStillOnItsWay) {
    const CallId call = held();
    EXPECT_EQ(ringer_.signal(room_id(), user("alice"), CallSignal::Cancel, call), user("alice"));
    EXPECT_EQ(plane_.take(), (std::vector{line(RingEvent::Cancelled, "alice", "alice"),
                                          line(RingEvent::Cancelled, "bob", "alice")}));
    EXPECT_EQ(ringer_.ticketed(room_id(), user("bob"), std::nullopt), std::nullopt);
    EXPECT_TRUE(plane_.take().empty());
    EXPECT_TRUE(ringer_.idle(room_id()));
}

TEST_F(GracedRingerTest, ADeposedOwnerForgetsAHeldCallWithoutAWord) {
    const CallId call = held();
    plane_.owning = false;
    clock_.advance(chat::RingLimits{}.answer_grace);
    ringer_.tick();
    EXPECT_TRUE(plane_.take().empty());
    EXPECT_TRUE(ringer_.idle(room_id()));
    EXPECT_EQ(ringer_.counters().orphaned, 1U);
    EXPECT_EQ(ringer_.counters().missed, 0U);
    // The answer that arrives after finds nothing to answer.
    EXPECT_EQ(ringer_.ticketed(room_id(), user("bob"), std::nullopt), std::nullopt);
    EXPECT_FALSE(ringer_.signal(room_id(), user("bob"), CallSignal::Decline, call));
}

TEST_F(RingerTest, TheMemberWhoDeclinedCallingBackFreesTheCallerTheyDeclined) {
    const CallId call = ring();
    ASSERT_TRUE(ringer_.signal(room_id(), user("bob"), CallSignal::Decline, call));
    ASSERT_TRUE(ringer_.ring_limited(room_id(), user("alice")));
    // Bob calls alice back and gives up: alice may ring him now, well inside the cooldown.
    const auto back = ringer_.ticketed(room_id(), user("bob"), kPair);
    ASSERT_TRUE(back && *back);
    ASSERT_TRUE(ringer_.signal(room_id(), user("bob"), CallSignal::Cancel, **back));
    EXPECT_FALSE(ringer_.ring_limited(room_id(), user("alice")));
    EXPECT_TRUE(ringer_.ticketed(room_id(), user("alice"), kPair).value_or(std::nullopt));
}

TEST(RingLimitsBounds, TheRingsAllowedPerWindowAreHeldBetweenOneAndWhatAHistoryHolds) {
    for (const auto& [asked, allowed] : std::vector<std::pair<std::uint32_t, std::size_t>>{
             {0, 1}, {100, chat::kMaxRingsPerWindow}}) {
        FakePlane plane;
        ulw::test::FakeClock clock;
        ulw::test::FakeRandom random;
        chat::Ringer ringer(plane, clock, random, chat::RingLimits{.rings_per_window = asked});
        std::size_t rung = 0;
        while (true) {
            const auto call = ringer.ticketed(room_id(), user("alice"), kPair);
            if (!call) {
                EXPECT_EQ(call.error().why, chat::RingRefusal::Why::Limited);
                break;
            }
            ASSERT_TRUE(*call);
            ++rung;
            ASSERT_TRUE(ringer.signal(room_id(), user("alice"), CallSignal::Cancel, **call));
            clock.advance(core::Millis{1});
        }
        EXPECT_EQ(rung, allowed) << asked;
    }
}

class HistoryBoundedRingerTest : public RingerTest {
protected:
    HistoryBoundedRingerTest() : RingerTest(chat::RingLimits{.max_histories = 1}) {}
};

TEST_F(HistoryBoundedRingerTest, ARoomPastTheRememberedRoomsIsBusyUntilAnOldOneIsForgotten) {
    const CallId call = ring();
    ASSERT_TRUE(ringer_.signal(room_id(), user("alice"), CallSignal::Cancel, call));
    const auto refused = ringer_.ticketed(room_id(kOtherRoom), user("alice"), kPair);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().why, chat::RingRefusal::Why::Busy);
    clock_.advance(chat::RingLimits{}.ring_window);
    EXPECT_TRUE(ringer_.ticketed(room_id(kOtherRoom), user("alice"), kPair).value_or(std::nullopt));
}

// A group chat's call (ADR-0095): alice calls bob, carol and dave.
const std::vector<core::UserId> kGroup{user("alice"), user("bob"), user("carol"), user("dave")};

class GroupRingerTest : public RingerTest {
protected:
    CallId ring_group(const std::vector<core::UserId>& members = kGroup) {
        const auto call =
            ringer_.ticketed(room_id(), user("alice"), members, chat::CallKind::Group);
        EXPECT_TRUE(call && *call);
        return call.value_or(std::nullopt).value_or(CallId::generate(clock_, random_));
    }
    std::optional<CallId> join(std::string_view who) {
        return ringer_.ticketed(room_id(), user(who), std::nullopt, chat::CallKind::Group)
            .value_or(std::nullopt);
    }
    // Everyone in kGroup hears `event` by `by`, in the list's order.
    static std::vector<std::string> all(RingEvent event, std::string_view by = {}) {
        std::vector<std::string> out;
        for (const core::UserId& u : kGroup) {
            out.push_back(line(event, u.view(), by));
        }
        return out;
    }
};

TEST_F(GroupRingerTest, AGroupCallRingsEveryOtherMemberAndEachAnswerIsHeardByAll) {
    const CallId call = ring_group();
    EXPECT_EQ(plane_.take(), all(RingEvent::Ringing));
    EXPECT_EQ(ringer_.kind_of(room_id()), chat::CallKind::Group);
    EXPECT_EQ(join("bob"), call);
    EXPECT_EQ(plane_.take(), all(RingEvent::Answered, "bob"));
    EXPECT_EQ(join("carol"), call);
    EXPECT_EQ(plane_.take(), all(RingEvent::Answered, "carol"));
    // Asking again, in the call already, says nothing.
    EXPECT_EQ(join("bob"), call);
    EXPECT_EQ(join("alice"), call);
    EXPECT_TRUE(plane_.take().empty());
    EXPECT_EQ(ringer_.joined(room_id()), (std::vector{user("alice"), user("bob"), user("carol")}));
    EXPECT_EQ(ringer_.counters().answered, 1U) << "a call is answered once";
}

TEST_F(GroupRingerTest, OnlyThoseStillRungHearItAgainAndMissItWhileTheCallGoesOn) {
    const CallId call = ring_group();
    plane_.take();
    ASSERT_EQ(join("bob"), call);
    plane_.take();
    const chat::RingLimits limits;
    clock_.advance(limits.announce_every);
    ringer_.tick();
    EXPECT_EQ(plane_.take(),
              (std::vector{line(RingEvent::Ringing, "carol"), line(RingEvent::Ringing, "dave")}));
    clock_.advance(limits.ring_timeout - limits.announce_every);
    ringer_.tick();
    // Each one's ring ran out; the call goes on for alice and bob.
    EXPECT_EQ(plane_.take(),
              (std::vector{line(RingEvent::Missed, "carol"), line(RingEvent::Missed, "dave")}));
    EXPECT_FALSE(ringer_.idle(room_id()));
    EXPECT_EQ(ringer_.counters().missed, 2U);
    // A member whose ring ran out may still come.
    EXPECT_EQ(join("dave"), call);
    EXPECT_EQ(plane_.take(), all(RingEvent::Answered, "dave"));
}

TEST_F(GroupRingerTest, NobodyAnsweringRingsOutForEveryone) {
    const CallId call = ring_group();
    plane_.take();
    clock_.advance(chat::RingLimits{}.ring_timeout);
    ringer_.tick();
    // The re-announcement due at the same time is not sent: the call rang out.
    EXPECT_EQ(plane_.take(), all(RingEvent::Missed));
    EXPECT_TRUE(ringer_.idle(room_id()));
    EXPECT_EQ(join("bob"), std::nullopt);
    EXPECT_FALSE(ringer_.signal(room_id(), user("bob"), CallSignal::Decline, call));
}

TEST_F(GroupRingerTest, ADeclineIsOneMembersAndEveryoneDecliningIsAMissedCall) {
    const CallId call = ring_group();
    plane_.take();
    EXPECT_EQ(ringer_.signal(room_id(), user("bob"), CallSignal::Decline, call), user("alice"));
    EXPECT_EQ(plane_.take(), all(RingEvent::Declined, "bob"));
    // Once, and only by someone rung.
    EXPECT_FALSE(ringer_.signal(room_id(), user("bob"), CallSignal::Decline, call));
    EXPECT_FALSE(ringer_.signal(room_id(), user("alice"), CallSignal::Decline, call));
    ASSERT_EQ(ringer_.signal(room_id(), user("carol"), CallSignal::Decline, call), user("alice"));
    plane_.take();
    ASSERT_EQ(ringer_.signal(room_id(), user("dave"), CallSignal::Decline, call), user("alice"));
    auto expected = all(RingEvent::Declined, "dave");
    const auto missed = all(RingEvent::Missed);
    expected.insert(expected.end(), missed.begin(), missed.end());
    EXPECT_EQ(plane_.take(), expected);
    EXPECT_TRUE(ringer_.idle(room_id()));
    // No cooldown on a group: a decline is one member's.
    EXPECT_FALSE(ringer_.ring_limited(room_id(), user("alice")));
}

TEST_F(GroupRingerTest, AMembersLeavingIsHeardAndTheLastOneOutEndsTheCall) {
    const CallId call = ring_group();
    ASSERT_EQ(join("bob"), call);
    plane_.take();
    // In a group call, ending is the caller's, for everyone, through may_end; leaving is anyone's.
    EXPECT_FALSE(ringer_.signal(room_id(), user("bob"), CallSignal::End, call));
    EXPECT_FALSE(ringer_.signal(room_id(), user("carol"), CallSignal::Leave, call));
    EXPECT_FALSE(ringer_.signal(room_id(), user("bob"), CallSignal::Expel, call));
    EXPECT_EQ(ringer_.signal(room_id(), user("alice"), CallSignal::Leave, call), user("alice"));
    EXPECT_EQ(plane_.take(), all(RingEvent::Left, "alice"));
    EXPECT_FALSE(ringer_.idle(room_id()));
    EXPECT_EQ(ringer_.signal(room_id(), user("bob"), CallSignal::Leave, call), user("alice"));
    auto expected = all(RingEvent::Left, "bob");
    const auto ended = all(RingEvent::Ended, "bob");
    expected.insert(expected.end(), ended.begin(), ended.end());
    EXPECT_EQ(plane_.take(), expected);
    EXPECT_TRUE(ringer_.idle(room_id()));
    EXPECT_EQ(ringer_.counters().left, 2U);
    EXPECT_EQ(ringer_.counters().emptied, 1U);
}

TEST_F(GroupRingerTest, ADirectCallHasNoLeaveNorExpel) {
    const CallId call = ring();
    EXPECT_EQ(ringer_.kind_of(room_id()), chat::CallKind::Direct);
    EXPECT_FALSE(ringer_.signal(room_id(), user("alice"), CallSignal::Leave, call));
    EXPECT_FALSE(ringer_.may_end(room_id(), user("alice"), call));
    EXPECT_FALSE(ringer_.expel(room_id(), user("alice"), call, user("bob")));
    EXPECT_TRUE(ringer_.fits(room_id(), user("carol")));
}

TEST_F(GroupRingerTest, OnlyTheCallerEndsAGroupCallForEveryone) {
    const CallId call = ring_group();
    ASSERT_EQ(join("bob"), call);
    plane_.take();
    const CallId other = CallId::generate(clock_, random_);
    EXPECT_FALSE(ringer_.may_end(room_id(), user("bob"), call));
    EXPECT_FALSE(ringer_.may_end(room_id(), user("alice"), other));
    EXPECT_EQ(ringer_.may_end(room_id(), user("alice"), call), chat::MediaStepNeeded::Close);
    // Nothing is said until the handler's write is done.
    EXPECT_TRUE(plane_.take().empty());
    ringer_.ended(room_id(), other, user("alice"));
    EXPECT_FALSE(ringer_.idle(room_id()));
    ringer_.ended(room_id(), call, user("alice"));
    EXPECT_EQ(plane_.take(), all(RingEvent::Ended, "alice"));
    EXPECT_TRUE(ringer_.idle(room_id()));
}

TEST_F(GroupRingerTest, TheCallerPutsAMemberOutWhoMayNotComeBackWhileTheCallLasts) {
    const CallId call = ring_group();
    ASSERT_EQ(join("bob"), call);
    plane_.take();
    // Not by a member who is not the caller, not the caller, not another call.
    EXPECT_FALSE(ringer_.expel(room_id(), user("bob"), call, user("carol")));
    EXPECT_FALSE(ringer_.expel(room_id(), user("alice"), call, user("alice")));
    EXPECT_FALSE(
        ringer_.expel(room_id(), user("alice"), CallId::generate(clock_, random_), user("bob")));
    EXPECT_FALSE(ringer_.expelled(room_id(), user("bob")));
    // Bob holds a ticket: the media room must move. Carol does not: nothing to move.
    EXPECT_EQ(ringer_.expel(room_id(), user("alice"), call, user("bob")),
              chat::MediaStepNeeded::Move);
    EXPECT_EQ(ringer_.expel(room_id(), user("alice"), call, user("carol")),
              chat::MediaStepNeeded::None);
    EXPECT_TRUE(ringer_.expelled(room_id(), user("bob")));
    EXPECT_TRUE(plane_.take().empty());
    ringer_.moved(room_id(), call, user("alice"), user("bob"));
    // Everyone still in it, those still rung, and bob himself.
    EXPECT_EQ(plane_.take(), (std::vector{line(RingEvent::Moved, "alice", "alice"),
                                          line(RingEvent::Moved, "dave", "alice"),
                                          line(RingEvent::Moved, "bob", "alice")}));
    const auto refused =
        ringer_.ticketed(room_id(), user("bob"), std::nullopt, chat::CallKind::Group);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().why, chat::RingRefusal::Why::Expelled);
    EXPECT_EQ(ringer_.counters().expelled, 2U);
    EXPECT_EQ(ringer_.joined(room_id()), std::vector{user("alice")});
}

TEST_F(GroupRingerTest, TheMovedNoticeNamesWhoWasPutOut) {
    const CallId call = ring_group();
    ASSERT_EQ(join("bob"), call);
    ASSERT_TRUE(ringer_.expel(room_id(), user("alice"), call, user("bob")));
    plane_.sent.clear();
    ringer_.moved(room_id(), call, std::nullopt, user("bob"));
    ASSERT_FALSE(plane_.sent.empty());
    for (const chat::CallNotice& n : plane_.sent) {
        EXPECT_EQ(n.event, RingEvent::Moved);
        EXPECT_EQ(n.subject, user("bob"));
        EXPECT_FALSE(n.by);
    }
}

TEST_F(GroupRingerTest, AMemberRemovedFromTheChatIsPutOutAsIfExpelled) {
    const CallId call = ring_group();
    ASSERT_EQ(join("bob"), call);
    plane_.take();
    EXPECT_EQ(ringer_.removed(room_id(), user("mallory")), chat::MediaStepNeeded::None);
    EXPECT_EQ(ringer_.removed(room_id(), user("bob")), chat::MediaStepNeeded::Move);
    EXPECT_EQ(ringer_.removed(room_id(), user("carol")), chat::MediaStepNeeded::None);
    EXPECT_TRUE(ringer_.expelled(room_id(), user("carol")));
    EXPECT_TRUE(plane_.take().empty());
    EXPECT_EQ(ringer_.removed(room_id(kOtherRoom), user("bob")), chat::MediaStepNeeded::None);
}

TEST_F(GroupRingerTest, ARemovalThatLeavesNobodyToRingIsAMissedCall) {
    ring_group({user("alice"), user("bob")});
    plane_.take();
    EXPECT_EQ(ringer_.removed(room_id(), user("bob")), chat::MediaStepNeeded::None);
    EXPECT_EQ(plane_.take(), std::vector{line(RingEvent::Missed, "alice")});
    EXPECT_TRUE(ringer_.idle(room_id()));
}

TEST_F(GroupRingerTest, ADirectCallLosingAMemberEnds) {
    ring();
    EXPECT_EQ(ringer_.removed(room_id(), user("bob")), chat::MediaStepNeeded::Close);
    EXPECT_EQ(ringer_.joined(room_id()), std::vector{user("alice")});
}

TEST_F(GroupRingerTest, AQuietGroupCallAsksWhetherAnyoneIsStillInIt) {
    const CallId call = ring_group();
    ASSERT_EQ(join("bob"), call);
    plane_.take();
    const chat::RingLimits limits;
    // Past the ring, before the hold: nothing to check.
    clock_.advance(limits.ring_timeout);
    ringer_.tick();
    plane_.take();
    EXPECT_TRUE(ringer_.take_checks().empty());
    clock_.advance(limits.answered_hold - limits.ring_timeout);
    ringer_.tick();
    const auto checks = ringer_.take_checks();
    ASSERT_EQ(checks.size(), 1U);
    EXPECT_EQ(checks[0].first, room_id());
    EXPECT_EQ(checks[0].second, call);
    // Asked once until answered.
    ringer_.tick();
    EXPECT_TRUE(ringer_.take_checks().empty());
    // The SFU did not answer: asked again a check later. Someone is in it: again later too.
    ringer_.occupied(room_id(), call, std::nullopt);
    clock_.advance(limits.occupancy_check - core::Millis{1});
    ringer_.tick();
    EXPECT_TRUE(ringer_.take_checks().empty());
    clock_.advance(core::Millis{1});
    ringer_.tick();
    ASSERT_EQ(ringer_.take_checks().size(), 1U);
    ringer_.occupied(room_id(), call, true);
    EXPECT_TRUE(plane_.take().empty());
    clock_.advance(limits.occupancy_check);
    ringer_.tick();
    ASSERT_EQ(ringer_.take_checks().size(), 1U);
    // An answer for another call, or one nobody asked for, changes nothing.
    ringer_.occupied(room_id(), CallId::generate(clock_, random_), false);
    EXPECT_FALSE(ringer_.idle(room_id()));
    // Nobody: the call is over for everyone.
    ringer_.occupied(room_id(), call, false);
    EXPECT_EQ(plane_.take(), all(RingEvent::Ended));
    EXPECT_TRUE(ringer_.idle(room_id()));
    EXPECT_EQ(ringer_.counters().emptied, 1U);
    ringer_.occupied(room_id(), call, false);
}

TEST_F(GroupRingerTest, ATicketPutsOffTheCheck) {
    const CallId call = ring_group();
    ASSERT_EQ(join("bob"), call);
    const chat::RingLimits limits;
    clock_.advance(limits.answered_hold - core::Millis{1});
    ASSERT_EQ(join("carol"), call);
    clock_.advance(core::Millis{1});
    ringer_.tick();
    EXPECT_TRUE(ringer_.take_checks().empty());
    clock_.advance(limits.answered_hold);
    ringer_.tick();
    EXPECT_EQ(ringer_.take_checks().size(), 1U);
}

TEST_F(GroupRingerTest, ABigGroupRingsItsFirstMembersAndAnyOtherMayJoin) {
    std::vector<core::UserId> members{user("alice")};
    for (std::size_t i = 0; i < chat::kMaxGroupCallees + 8; ++i) {
        members.push_back(user("m" + std::to_string(100 + i)));
    }
    const CallId call = ring_group(members);
    EXPECT_EQ(plane_.take().size(), chat::kMaxGroupCallees + 1);
    // One not rung joins: everyone hears it, the newcomer too.
    EXPECT_EQ(join("m139"), call);
    const auto heard = plane_.take();
    EXPECT_EQ(heard.size(), chat::kMaxGroupCallees + 2);
    EXPECT_EQ(heard.back(), line(RingEvent::Answered, "m139", "m139"));
}

TEST_F(GroupRingerTest, AGroupCallHoldsSoManyMembersInIt) {
    std::vector<core::UserId> members{user("alice")};
    for (std::size_t i = 0; i < chat::kMaxGroupJoined + 1; ++i) {
        members.push_back(user("m" + std::to_string(100 + i)));
    }
    const CallId call = ring_group(members);
    for (std::size_t i = 0; i + 1 < chat::kMaxGroupJoined; ++i) {
        ASSERT_EQ(join("m" + std::to_string(100 + i)), call);
    }
    const std::string last = "m" + std::to_string(100 + chat::kMaxGroupJoined);
    EXPECT_FALSE(ringer_.fits(room_id(), user(last)));
    EXPECT_TRUE(ringer_.fits(room_id(), user("m100")));
    const auto refused =
        ringer_.ticketed(room_id(), user(last), std::nullopt, chat::CallKind::Group);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().why, chat::RingRefusal::Why::Busy);
}

TEST_F(GroupRingerTest, AGroupCalleeAnsweringJustBeforeTheTimeoutIsHeld) {
    const CallId call = ring_group();
    plane_.take();
    const chat::RingLimits limits;
    clock_.advance(limits.ring_timeout - core::Millis{1});
    ringer_.answering(room_id(), user("carol"));
    clock_.advance(core::Millis{1});
    ringer_.tick();
    EXPECT_TRUE(plane_.take().empty());
    EXPECT_EQ(ringer_.counters().graced, 1U);
    EXPECT_EQ(join("carol"), call);
    EXPECT_EQ(plane_.take(), all(RingEvent::Answered, "carol"));
    clock_.advance(limits.answer_grace);
    ringer_.tick();
    EXPECT_EQ(plane_.take(),
              (std::vector{line(RingEvent::Missed, "bob"), line(RingEvent::Missed, "dave")}));
}

TEST_F(GroupRingerTest, ADeposedOwnerForgetsAGroupCallToo) {
    ring_group();
    plane_.take();
    plane_.owning = false;
    clock_.advance(chat::RingLimits{}.announce_every);
    ringer_.tick();
    EXPECT_TRUE(plane_.take().empty());
    EXPECT_TRUE(ringer_.idle(room_id()));
    EXPECT_EQ(ringer_.rooms().size(), 0U);
}

TEST(CallNoticeCodec, AMovedNoticeCarriesWhoWasPutOut) {
    const ulw::test::FakeClock clock;
    ulw::test::FakeRandom random;
    const chat::CallNotice sent{.event = RingEvent::Moved,
                                .to = user("alice"),
                                .room = room_id(),
                                .call = CallId::generate(clock, random),
                                .from = user("alice"),
                                .by = std::nullopt,
                                .expires_at = core::WallTime{core::Millis{5}},
                                .subject = user("bob")};
    const auto bytes = chat::encode_notice(sent);
    const auto back = chat::decode_notice(bytes);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->event, RingEvent::Moved);
    EXPECT_EQ(back->subject, user("bob"));
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        EXPECT_FALSE(chat::decode_notice(std::span(bytes).first(cut))) << cut;
    }
    // Any other event carries none, and reads none.
    chat::CallNotice left = sent;
    left.event = RingEvent::Left;
    const auto left_back = chat::decode_notice(chat::encode_notice(left));
    ASSERT_TRUE(left_back);
    EXPECT_FALSE(left_back->subject);
}

TEST(CallNoticeCodec, ANoticeComesBackAsItWasSentAndAnythingElseIsRefused) {
    const ulw::test::FakeClock clock;
    ulw::test::FakeRandom random;
    const chat::CallNotice sent{.event = RingEvent::Declined,
                                .to = user("auth0|alice"),
                                .room = room_id(),
                                .call = CallId::generate(clock, random),
                                .from = user("auth0|alice"),
                                .by = user("auth0|bob"),
                                .expires_at = core::WallTime{core::Millis{1'790'000'000'123}}};
    const auto bytes = chat::encode_notice(sent);
    const auto back = chat::decode_notice(bytes);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->event, sent.event);
    EXPECT_EQ(back->to, sent.to);
    EXPECT_EQ(back->room, sent.room);
    EXPECT_EQ(back->call, sent.call);
    EXPECT_EQ(back->from, sent.from);
    EXPECT_EQ(back->by, sent.by);
    EXPECT_EQ(back->expires_at, sent.expires_at);

    chat::CallNotice ringing = sent;
    ringing.event = RingEvent::Ringing;
    ringing.by.reset();
    const auto no_by = chat::decode_notice(chat::encode_notice(ringing));
    ASSERT_TRUE(no_by);
    EXPECT_FALSE(no_by->by);

    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        EXPECT_FALSE(chat::decode_notice(std::span(bytes).first(cut))) << cut;
    }
    auto trailing = bytes;
    trailing.push_back(std::byte{0});
    EXPECT_FALSE(chat::decode_notice(trailing));
    auto other_layout = bytes;
    other_layout[0] = std::byte{9};
    EXPECT_FALSE(chat::decode_notice(other_layout));
    auto unknown_event = bytes;
    unknown_event[1] = std::byte{8};
    EXPECT_FALSE(chat::decode_notice(unknown_event));
}

} // namespace

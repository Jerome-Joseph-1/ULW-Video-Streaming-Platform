#include "core/util/json.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "notify_sender.hpp"
#include "support/fake_clock.hpp"
#include "support/http_test_server.hpp"
#include "support/reactor_harness.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using ulw::test::HttpTestServer;
using ulw::test::pump_until;
using ulw::test::Reply;
using ulw::test::ServedRequest;

constexpr std::string_view kSecret = "a-secret-of-at-least-thirty-two-bytes!";

class NotifySenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto r = net::make_reactor(net::ReactorKind::Epoll, reactor_clock, 1024);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        auto m = infra::curl::Multi::create(*reactor);
        ASSERT_TRUE(m);
        multi = std::move(*m);
    }
    void TearDown() override {
        sender.reset();
        multi.reset();
        reactor.reset();
    }

    void make(const std::string& url, chat::SenderLimits limits = {}) {
        sender = std::make_unique<chat::HookSender>(*reactor, *multi, clock, url,
                                                    std::string(kSecret), limits);
    }
    bool idle() {
        return pump_until(
            *reactor, [&] { return !sender->busy() && sender->queued() == 0; },
            std::chrono::seconds(10));
    }

    os::SystemClock reactor_clock;
    ulw::test::FakeClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<chat::HookSender> sender;
};

TEST(NotifySignature, IsHmacSha256OfTheTimeAndTheBody) {
    // printf '%s' '1767225600.{"events":[]}' | openssl dgst -sha256 -hmac secret
    EXPECT_EQ(chat::notify_signature("secret", 1'767'225'600, R"({"events":[]})"),
              "t=1767225600,v1=9ad04090ab1beebfe548dca4c8329f79a53d98be77244b3baea55fe1de9d5412");
    // Another body, another time, another secret: another signature.
    EXPECT_NE(chat::notify_signature("secret", 1, "a"), chat::notify_signature("secret", 1, "b"));
    EXPECT_NE(chat::notify_signature("secret", 1, "a"), chat::notify_signature("secret", 2, "a"));
    EXPECT_NE(chat::notify_signature("secret", 1, "a"), chat::notify_signature("other", 1, "a"));
}

TEST_F(NotifySenderTest, EventsGoOutBatchedAndSignedInTheOrderOffered) {
    const HttpTestServer receiver(
        [](const ServedRequest&) { return Reply{.status = 204, .headers = {}, .body = {}}; });
    make(receiver.base_url() + "/ulw/events", {.batch = 2});
    sender->offer(R"({"n":1})");
    sender->offer(R"({"n":2})");
    sender->offer(R"({"n":3})");
    ASSERT_TRUE(idle());
    const auto requests = receiver.requests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[0].method, "POST");
    EXPECT_EQ(requests[0].path(), "/ulw/events");
    EXPECT_EQ(requests[0].header("content-type").value_or(""), "application/json");
    // The first went at once, alone; the two that waited behind it went together.
    EXPECT_EQ(requests[0].body, R"({"events":[{"n":1}]})");
    EXPECT_EQ(requests[1].body, R"({"events":[{"n":2},{"n":3}]})");
    for (const ServedRequest& r : requests) {
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(clock.wall_now().time_since_epoch())
                .count();
        EXPECT_EQ(r.header("ulw-signature").value_or(""),
                  chat::notify_signature(kSecret, seconds, r.body));
        EXPECT_TRUE(core::json::parse(r.body));
    }
    EXPECT_EQ(sender->counters().sent, 3U);
    EXPECT_EQ(sender->counters().requests, 2U);
    EXPECT_EQ(sender->counters().failed, 0U);
}

TEST_F(NotifySenderTest, AFullQueueDropsWhatComesAndCountsIt) {
    const HttpTestServer receiver(
        [](const ServedRequest&) { return Reply{.status = 200, .headers = {}, .body = {}}; });
    make(receiver.base_url(), {.max_queued = 2, .batch = 1});
    // The first is taken off the queue at once; two wait; the fourth finds the queue full.
    for (int i = 0; i < 4; ++i) {
        sender->offer("{}");
    }
    EXPECT_EQ(sender->counters().dropped, 1U);
    ASSERT_TRUE(idle());
    EXPECT_EQ(sender->counters().sent, 3U);
    EXPECT_EQ(receiver.request_count(), 3U);
}

TEST_F(NotifySenderTest, ARefusedOrSlowRequestDropsItsEventsAndTheNextGoesOut) {
    int served = 0;
    const HttpTestServer receiver([&served](const ServedRequest&) {
        ++served;
        if (served == 1) {
            return Reply{.status = 500, .headers = {}, .body = {}};
        }
        if (served == 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }
        return Reply{.status = 202, .headers = {}, .body = {}};
    });
    make(receiver.base_url(), {.batch = 1, .timeout = core::Millis{200}});
    sender->offer(R"({"n":1})");
    sender->offer(R"({"n":2})");
    ASSERT_TRUE(idle());
    EXPECT_EQ(sender->counters().failed, 2U);
    // The test receiver serves one connection at a time: offer the last once the slow one is done.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    sender->offer(R"({"n":3})");
    ASSERT_TRUE(idle());
    EXPECT_EQ(sender->counters().failed, 2U);
    EXPECT_EQ(sender->counters().failed_requests, 2U);
    EXPECT_EQ(sender->counters().sent, 1U);
}

TEST_F(NotifySenderTest, AnUnreachableReceiverFailsWithoutBlockingTheCaller) {
    // Nothing listens on port 1.
    make("http://127.0.0.1:1/events", {.timeout = core::Millis{500}});
    sender->offer("{}");
    ASSERT_TRUE(idle());
    EXPECT_EQ(sender->counters().failed, 1U);
}

} // namespace

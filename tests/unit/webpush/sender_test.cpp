#include "infra/webpush/sender.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_push_transport.hpp"

#include <algorithm>
#include <deque>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace infra::webpush;
using namespace std::chrono_literals;

using FakeTransport = ulw::test::FakePushTransport;
using ulw::test::push_header;

class SenderTest : public ::testing::Test {
protected:
    SenderTest()
        : key_(VapidKey::from_private(PrivateKey{7}).value()),
          sender_(make(SenderLimits{.max_queue = 4,
                                    .max_in_flight = 2,
                                    .max_attempts = 3,
                                    .first_backoff = 1'000ms,
                                    .max_backoff = 4'000ms,
                                    .subject = "mailto:ops@example.com"})) {
        sender_->on_gone(
            [this](const std::string& endpoint) noexcept { gone_.push_back(endpoint); });
    }

    std::unique_ptr<PushSender> make(SenderLimits limits) {
        auto transport = std::make_unique<FakeTransport>();
        transport_ = transport.get();
        return std::make_unique<PushSender>(std::move(transport), key_, clock_, std::move(limits));
    }

    PushMessage message(std::string endpoint = "https://fcm.googleapis.com/fcm/send/a",
                        core::Millis life = 45'000ms) {
        return PushMessage{.endpoint = std::move(endpoint),
                           .audience = "https://fcm.googleapis.com",
                           .body = {1, 2, 3},
                           .deadline = clock_.now() + life,
                           .urgency = Urgency::High};
    }

    ulw::test::FakeClock clock_;
    VapidKey key_;
    FakeTransport* transport_ = nullptr;
    std::vector<std::string> gone_;
    std::unique_ptr<PushSender> sender_;
};

TEST_F(SenderTest, PostsTheMessageWithItsHeaders) {
    ASSERT_TRUE(sender_->enqueue(message()));
    ASSERT_EQ(transport_->posts.size(), 1U);
    const PushRequest& r = transport_->posts.front().request;
    EXPECT_EQ(r.url, "https://fcm.googleapis.com/fcm/send/a");
    EXPECT_EQ(r.body, (std::vector<std::uint8_t>{1, 2, 3}));
    EXPECT_EQ(push_header(r, "TTL"), "45");
    EXPECT_EQ(push_header(r, "Urgency"), "high");
    EXPECT_EQ(push_header(r, "Content-Encoding"), "aes128gcm");
    EXPECT_EQ(push_header(r, "Content-Type"), "application/octet-stream");
    const auto auth = push_header(r, "Authorization");
    ASSERT_TRUE(auth.has_value());
    EXPECT_TRUE(auth->starts_with("vapid t="));
    EXPECT_TRUE(auth->ends_with("k=" + key_.public_key()));
    EXPECT_EQ(sender_->in_flight(), 1U);
    transport_->status(201);
    EXPECT_EQ(sender_->counters().delivered, 1U);
    EXPECT_EQ(sender_->counters().queued, 1U);
    EXPECT_EQ(sender_->in_flight(), 0U);
}

TEST_F(SenderTest, TheTtlIsWhatIsLeftRoundedUpAndNeverZero) {
    ASSERT_TRUE(sender_->enqueue(message("https://fcm.googleapis.com/a", 1'500ms)));
    EXPECT_EQ(push_header(transport_->posts.back().request, "TTL"), "2");
    ASSERT_TRUE(sender_->enqueue(message("https://fcm.googleapis.com/b", 1ms)));
    EXPECT_EQ(push_header(transport_->posts.back().request, "TTL"), "1");
}

TEST_F(SenderTest, AGoneSubscriptionIsHandedBackAndNotRetried) {
    ASSERT_TRUE(sender_->enqueue(message("https://fcm.googleapis.com/gone")));
    ASSERT_TRUE(sender_->enqueue(message("https://fcm.googleapis.com/not-found")));
    transport_->status(410);
    transport_->status(404);
    EXPECT_EQ(gone_, (std::vector<std::string>{"https://fcm.googleapis.com/gone",
                                               "https://fcm.googleapis.com/not-found"}));
    EXPECT_EQ(sender_->counters().gone, 2U);
    clock_.advance(10'000ms);
    sender_->tick();
    EXPECT_TRUE(transport_->posts.empty());
}

TEST_F(SenderTest, OtherClientErrorsAreRejectedWithoutARetry) {
    for (const int status : {400, 401, 403, 413}) {
        ASSERT_TRUE(sender_->enqueue(message()));
        transport_->status(status);
    }
    EXPECT_EQ(sender_->counters().rejected, 4U);
    clock_.advance(10'000ms);
    sender_->tick();
    EXPECT_TRUE(transport_->posts.empty());
    EXPECT_EQ(transport_->total, 4U);
    EXPECT_TRUE(gone_.empty());
}

TEST_F(SenderTest, AServerErrorIsRetriedWithBackoffUpToTheAttemptLimit) {
    ASSERT_TRUE(sender_->enqueue(message()));
    transport_->status(503);
    EXPECT_EQ(sender_->counters().retried, 1U);
    EXPECT_EQ(sender_->queued(), 1U);
    // Not before its backoff.
    clock_.advance(999ms);
    sender_->tick();
    EXPECT_TRUE(transport_->posts.empty());
    clock_.advance(1ms);
    sender_->tick();
    ASSERT_EQ(transport_->posts.size(), 1U);
    // The second attempt's TTL is what is left.
    EXPECT_EQ(push_header(transport_->posts.front().request, "TTL"), "44");
    transport_->status(500);
    clock_.advance(1'999ms);
    sender_->tick();
    EXPECT_TRUE(transport_->posts.empty());
    clock_.advance(1ms);
    sender_->tick();
    ASSERT_EQ(transport_->posts.size(), 1U);
    // The third attempt was the last.
    transport_->status(502);
    EXPECT_EQ(sender_->counters().failed, 1U);
    EXPECT_EQ(sender_->counters().retried, 2U);
    EXPECT_EQ(sender_->queued(), 0U);
    clock_.advance(60'000ms);
    sender_->tick();
    EXPECT_EQ(transport_->total, 3U);
}

TEST_F(SenderTest, TooManyRequestsWaitsForRetryAfter) {
    ASSERT_TRUE(sender_->enqueue(message()));
    transport_->status(429, core::Seconds{5});
    clock_.advance(4'999ms);
    sender_->tick();
    EXPECT_TRUE(transport_->posts.empty());
    clock_.advance(1ms);
    sender_->tick();
    ASSERT_EQ(transport_->posts.size(), 1U);
    transport_->status(201);
    EXPECT_EQ(sender_->counters().delivered, 1U);
}

TEST_F(SenderTest, ARetryThatWouldLandPastTheDeadlineIsNotScheduled) {
    ASSERT_TRUE(sender_->enqueue(message("https://fcm.googleapis.com/a", 10'000ms)));
    transport_->status(503, core::Seconds{30});
    EXPECT_EQ(sender_->counters().failed, 1U);
    EXPECT_EQ(sender_->counters().retried, 0U);
    EXPECT_EQ(sender_->queued(), 0U);
}

TEST_F(SenderTest, NetworkFailuresAreNotRetried) {
    for (const PushFailure f : {PushFailure::Network, PushFailure::Tls, PushFailure::Local}) {
        ASSERT_TRUE(sender_->enqueue(message()));
        transport_->answer(std::unexpected(f));
    }
    ASSERT_TRUE(sender_->enqueue(message()));
    transport_->answer(std::unexpected(PushFailure::AddressRefused));
    EXPECT_EQ(sender_->counters().failed, 3U);
    EXPECT_EQ(sender_->counters().refused, 1U);
    clock_.advance(10'000ms);
    sender_->tick();
    EXPECT_EQ(transport_->total, 4U);
}

TEST_F(SenderTest, AtMostMaxInFlightAtOnceAndTheQueueIsBounded) {
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(sender_->enqueue(message("https://fcm.googleapis.com/" + std::to_string(i))));
    }
    EXPECT_EQ(transport_->posts.size(), 2U);
    EXPECT_EQ(sender_->queued(), 2U);
    EXPECT_EQ(sender_->in_flight(), 2U);
    // max_queue counts what waits, not what is in flight.
    ASSERT_TRUE(sender_->enqueue(message("https://fcm.googleapis.com/4")));
    ASSERT_TRUE(sender_->enqueue(message("https://fcm.googleapis.com/5")));
    EXPECT_FALSE(sender_->enqueue(message("https://fcm.googleapis.com/6")));
    EXPECT_EQ(sender_->counters().dropped, 1U);
    // A place freed starts the next, oldest first.
    transport_->status(201);
    ASSERT_EQ(transport_->posts.size(), 2U);
    EXPECT_EQ(transport_->posts.back().request.url, "https://fcm.googleapis.com/2");
}

TEST_F(SenderTest, AMessagePastItsDeadlineWhileQueuedIsNotSent) {
    ASSERT_TRUE(sender_->enqueue(message("https://fcm.googleapis.com/0")));
    ASSERT_TRUE(sender_->enqueue(message("https://fcm.googleapis.com/1")));
    ASSERT_TRUE(sender_->enqueue(message("https://fcm.googleapis.com/2", 1'000ms)));
    clock_.advance(1'000ms);
    transport_->status(201);
    EXPECT_EQ(sender_->counters().expired, 1U);
    EXPECT_EQ(transport_->posts.size(), 1U);
    EXPECT_EQ(transport_->total, 2U);
}

TEST_F(SenderTest, ARequestTheTransportCannotStartIsCountedFailed) {
    transport_->refuse_next = true;
    ASSERT_TRUE(sender_->enqueue(message()));
    EXPECT_EQ(sender_->counters().failed, 1U);
    EXPECT_EQ(sender_->in_flight(), 0U);
}

TEST_F(SenderTest, WithoutAGoneHandlerAGoneEndpointIsOnlyCounted) {
    sender_->on_gone(nullptr);
    ASSERT_TRUE(sender_->enqueue(message()));
    transport_->status(410);
    EXPECT_EQ(sender_->counters().gone, 1U);
    EXPECT_TRUE(gone_.empty());
}

TEST_F(SenderTest, RequestsInFlightAreCancelledWithTheSender) {
    ASSERT_TRUE(sender_->enqueue(message()));
    sender_.reset();
    // The fake transport went with it; nothing called back.
    EXPECT_TRUE(gone_.empty());
}

TEST(Urgency, NamesRfc8030sValues) {
    EXPECT_EQ(to_string(Urgency::VeryLow), "very-low");
    EXPECT_EQ(to_string(Urgency::Low), "low");
    EXPECT_EQ(to_string(Urgency::Normal), "normal");
    EXPECT_EQ(to_string(Urgency::High), "high");
}

} // namespace

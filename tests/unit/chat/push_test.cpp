#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"
#include "infra/webpush/ece.hpp"

#include "push.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_push_transport.hpp"
#include "support/fake_random.hpp"
#include "support/memory_push_store.hpp"

#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using chat::ClientId;
using infra::webpush::KeyPair;

constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
constexpr std::string_view kCall = "01a0eb86-6cca-7dce-84cc-3bb47615f9aa";
constexpr std::string_view kDevice = "01a0eb86-6cca-7dce-84cc-3bb47615f9d1";
constexpr std::string_view kOtherDevice = "01a0eb86-6cca-7dce-84cc-3bb47615f9d2";
constexpr std::string_view kEndpoint = "https://fcm.googleapis.com/fcm/send/abc:APA91";

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

core::DeviceId device(std::string_view id = kDevice) {
    return *core::DeviceId::parse(id);
}

class RecordingClient final : public chat::IClient {
public:
    bool push(std::string_view text) noexcept override {
        heard.emplace_back(text);
        return true;
    }
    [[nodiscard]] std::size_t unsent_bytes() const noexcept override { return 0; }
    void allocation_failed() noexcept override { ++failures; }

    std::string take() {
        if (heard.empty()) {
            return {};
        }
        std::string first = heard.front();
        heard.erase(heard.begin());
        return first;
    }

    std::vector<std::string> heard;
    int failures = 0;
};

class Clients final : public chat::IClientLookup {
public:
    [[nodiscard]] chat::IClient* client(ClientId id) noexcept override {
        const auto it = clients.find(id.value);
        return it == clients.end() ? nullptr : it->second;
    }
    std::map<std::uint64_t, chat::IClient*> clients;
};

class PushTest : public ::testing::Test {
protected:
    PushTest()
        : key_(infra::webpush::VapidKey::from_private(infra::webpush::PrivateKey{9}).value()),
          ua_(infra::webpush::generate_key_pair().value()) {
        auto transport = std::make_unique<ulw::test::FakePushTransport>();
        transport_ = transport.get();
        sender_ = std::make_unique<infra::webpush::PushSender>(
            std::move(transport), key_, clock_, random_,
            infra::webpush::SenderLimits{.subject = "mailto:ops@example.com"});
        make(chat::PushLimits{
            .max_per_user = 3, .max_writes = 3, .max_writes_per_user = 2, .max_lookups = 2});
        clients_.clients[2] = &bob_;
        clients_.clients[1] = &alice_;
        auth_.fill(0x42);
    }

    void make(chat::PushLimits limits,
              infra::webpush::PushHosts hosts = infra::webpush::PushHosts::defaults()) {
        push_.reset();
        push_ = std::make_unique<chat::Push>(chat::PushDeps{.store = store_,
                                                            .sender = *sender_,
                                                            .key = key_,
                                                            .hosts = std::move(hosts),
                                                            .limits = limits},
                                             clients_, clock_);
    }

    chat::PushSubscribe subscription(std::string endpoint = std::string(kEndpoint),
                                     std::string_view dev = kDevice) {
        return chat::PushSubscribe{.device = device(dev),
                                   .endpoint = std::move(endpoint),
                                   .p256dh = ua_.public_key,
                                   .auth = auth_};
    }

    // bob's subscription, stored directly.
    void store_for(std::string_view who, std::string endpoint = std::string(kEndpoint),
                   std::string_view dev = kDevice) {
        store_.save(user(who),
                    core::ports::PushSubscription{.device = device(dev),
                                                  .endpoint = std::move(endpoint),
                                                  .p256dh = ua_.public_key,
                                                  .auth = auth_},
                    10, [](core::ports::PushResult<void>) noexcept {});
        store_.flush();
    }

    chat::CallPush ring(std::vector<core::UserId> callees = {user("bob")}) {
        return chat::CallPush{.room = *core::RoomId::parse(kRoom),
                              .call = *chat::CallId::parse(kCall),
                              .from = user("alice"),
                              .callees = std::move(callees),
                              .expires_at = clock_.wall_now() + 45s,
                              .deadline = clock_.now() + 45s};
    }

    // What a browser holding the subscription's key reads out of a push.
    std::string decrypted(const infra::webpush::PushRequest& r) {
        const auto plain = infra::webpush::decrypt(ua_.private_key, auth_, r.body);
        EXPECT_TRUE(plain.has_value());
        return plain ? std::string(plain->begin(), plain->end()) : std::string();
    }

    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
    infra::webpush::VapidKey key_;
    KeyPair ua_;
    infra::webpush::AuthSecret auth_{};
    ulw::test::MemoryPushStore store_;
    ulw::test::FakePushTransport* transport_ = nullptr;
    std::unique_ptr<infra::webpush::PushSender> sender_;
    Clients clients_;
    RecordingClient alice_;
    RecordingClient bob_;
    std::unique_ptr<chat::Push> push_;
};

TEST_F(PushTest, TheKeyIsTheVapidPublicKey) {
    push_->key(alice_);
    EXPECT_EQ(alice_.take(), R"({"type":"push_key","key":")" + key_.public_key() + R"("})");
}

TEST_F(PushTest, ASubscriptionIsSavedAndAnswered) {
    push_->subscribe(ClientId{1}, user("alice"), subscription());
    EXPECT_TRUE(alice_.heard.empty());
    store_.flush();
    EXPECT_EQ(alice_.take(),
              R"({"type":"push_subscribed","device":")" + std::string(kDevice) + R"("})");
    ASSERT_EQ(store_.rows.size(), 1U);
    EXPECT_EQ(store_.rows[0].user, user("alice"));
    EXPECT_EQ(store_.rows[0].subscription.endpoint, kEndpoint);
    EXPECT_EQ(store_.rows[0].subscription.p256dh, ua_.public_key);
    EXPECT_EQ(push_->counters().subscribed, 1U);

    push_->unsubscribe(ClientId{1}, user("alice"), chat::PushUnsubscribe{.device = device()});
    store_.flush();
    EXPECT_EQ(alice_.take(),
              R"({"type":"push_unsubscribed","device":")" + std::string(kDevice) + R"("})");
    EXPECT_TRUE(store_.rows.empty());
    EXPECT_EQ(push_->counters().unsubscribed, 1U);
}

TEST_F(PushTest, RefusesEndpointsAndKeysBeforeTheStoreHearsOfThem) {
    const auto refused = [&](const chat::PushSubscribe& s, std::string_view reason) {
        push_->subscribe(ClientId{1}, user("alice"), s);
        EXPECT_EQ(alice_.take(), R"({"type":"error","reason":")" + std::string(reason) +
                                     R"(","device":")" + std::string(kDevice) + R"("})");
    };
    refused(subscription("http://fcm.googleapis.com/x"), "bad_endpoint");
    refused(subscription("https://169.254.169.254/latest/meta-data"), "bad_endpoint");
    refused(subscription("https://push.attacker.example/x"), "push_host_not_allowed");
    auto off_curve = subscription();
    off_curve.p256dh[64] ^= 1U;
    refused(off_curve, "bad_key");
    EXPECT_EQ(store_.saves, 0U);
    EXPECT_EQ(push_->counters().refused, 4U);
}

TEST_F(PushTest, WritesWaitingAreCappedPerUserAndPerNode) {
    const std::string busy =
        R"({"type":"error","reason":"busy","device":")" + std::string(kDevice) + R"("})";
    push_->subscribe(ClientId{1}, user("alice"), subscription());
    push_->subscribe(ClientId{1}, user("alice"), subscription());
    // Two of alice's are waiting: her third, of either kind, is turned away.
    push_->subscribe(ClientId{1}, user("alice"), subscription());
    push_->unsubscribe(ClientId{1}, user("alice"), chat::PushUnsubscribe{.device = device()});
    EXPECT_EQ(alice_.take(), busy);
    EXPECT_EQ(alice_.take(), busy);
    // Another user is not held up by her, until the node's three are waiting.
    push_->unsubscribe(ClientId{2}, user("bob"), chat::PushUnsubscribe{.device = device()});
    push_->unsubscribe(ClientId{2}, user("bob"), chat::PushUnsubscribe{.device = device()});
    EXPECT_EQ(bob_.take(), busy);
    EXPECT_EQ(push_->counters().busy, 3U);
    store_.flush();
    EXPECT_EQ(alice_.heard.size(), 2U);
    EXPECT_EQ(bob_.heard.size(), 1U);
    alice_.heard.clear();
    // Answered, they are counted no more.
    push_->subscribe(ClientId{1}, user("alice"), subscription());
    push_->subscribe(ClientId{1}, user("alice"), subscription());
    store_.flush();
    EXPECT_EQ(alice_.heard.size(), 2U);
    EXPECT_EQ(push_->counters().busy, 3U);
}

TEST_F(PushTest, AWriteTheStoreThrowsOnIsNotCounted) {
    store_.throw_next = true;
    EXPECT_THROW(push_->subscribe(ClientId{1}, user("alice"), subscription()), std::bad_alloc);
    store_.throw_next = true;
    EXPECT_THROW(
        push_->unsubscribe(ClientId{1}, user("alice"), chat::PushUnsubscribe{.device = device()}),
        std::bad_alloc);
    // Nothing leaked: alice still has both her places.
    push_->subscribe(ClientId{1}, user("alice"), subscription());
    push_->subscribe(ClientId{1}, user("alice"), subscription());
    EXPECT_EQ(push_->counters().busy, 0U);
    store_.flush();
    EXPECT_EQ(alice_.heard.size(), 2U);
}

TEST_F(PushTest, TheStoredEndpointIsCanonical) {
    push_->subscribe(ClientId{1}, user("alice"),
                     subscription("HTTPS://FCM.GoogleAPIs.com:443/fcm/send/abc:APA91"));
    store_.flush();
    ASSERT_EQ(store_.rows.size(), 1U);
    EXPECT_EQ(store_.rows[0].subscription.endpoint, kEndpoint);
}

TEST_F(PushTest, EveryRingPushIsTheSameSizeWhoeverCalls) {
    store_for("bob");
    push_->ringing(ring());
    auto long_caller = ring();
    long_caller.from = user(std::string(100, 'z'));
    push_->ringing(long_caller);
    store_.flush();
    ASSERT_EQ(transport_->posts.size(), 2U);
    EXPECT_EQ(transport_->posts[0].request.body.size(), 86U + 512U + 16U);
    EXPECT_EQ(transport_->posts[1].request.body.size(), transport_->posts[0].request.body.size());
    EXPECT_NE(decrypted(transport_->posts[1].request).find(std::string(100, 'z')),
              std::string::npos);
}

TEST_F(PushTest, AStoreFailureIsUnavailableAndAGoneClientHearsNothing) {
    store_.fail = true;
    push_->subscribe(ClientId{1}, user("alice"), subscription());
    store_.flush();
    EXPECT_EQ(alice_.take(), R"({"type":"error","reason":"unavailable","device":")" +
                                 std::string(kDevice) + R"("})");
    EXPECT_EQ(push_->counters().store_failures, 1U);
    store_.fail = false;
    push_->subscribe(ClientId{1}, user("alice"), subscription());
    clients_.clients.erase(1);
    store_.flush();
    EXPECT_TRUE(alice_.heard.empty());
    // A client already gone asks nothing.
    push_->subscribe(ClientId{1}, user("alice"), subscription());
    push_->unsubscribe(ClientId{1}, user("alice"), chat::PushUnsubscribe{.device = device()});
    EXPECT_EQ(store_.waiting(), 0U);
}

TEST_F(PushTest, ARingPushesEveryDeviceOfEachCalleeWhatTheirSocketsHear) {
    store_for("bob", std::string(kEndpoint), kDevice);
    store_for("bob", "https://updates.push.services.mozilla.com/wpush/v2/x", kOtherDevice);
    store_for("carol", "https://web.push.apple.com/abc", kDevice);
    push_->ringing(ring({user("bob"), user("carol")}));
    EXPECT_EQ(push_->counters().lookups, 2U);
    store_.flush();
    ASSERT_EQ(transport_->posts.size(), 3U);
    EXPECT_EQ(push_->counters().messages, 3U);
    const std::string expected = R"({"type":"call_ringing","room":")" + std::string(kRoom) +
                                 R"(","call":")" + std::string(kCall) +
                                 R"(","from":"alice","expires_at":1767225645})";
    std::vector<std::string> urls;
    for (const auto& post : transport_->posts) {
        urls.push_back(post.request.url);
        EXPECT_EQ(decrypted(post.request), expected);
        EXPECT_EQ(ulw::test::push_header(post.request, "Urgency"), "high");
        EXPECT_EQ(ulw::test::push_header(post.request, "TTL"), "45");
    }
    EXPECT_EQ(urls,
              (std::vector<std::string>{"https://updates.push.services.mozilla.com/wpush/v2/x",
                                        std::string(kEndpoint), "https://web.push.apple.com/abc"}));
    // Each audience is its own push service's origin.
    const auto auth = ulw::test::push_header(transport_->posts[2].request, "Authorization");
    ASSERT_TRUE(auth.has_value());
    const std::string token(auth->substr(8, auth->find(',') - 8));
    const std::string claims =
        infra::auth::decode_base64url(
            token.substr(token.find('.') + 1, token.rfind('.') - token.find('.') - 1))
            .value_or("");
    EXPECT_NE(claims.find(R"("aud":"https://web.push.apple.com")"), std::string::npos);
}

TEST_F(PushTest, AGoneSubscriptionIsForgotten) {
    store_for("bob");
    push_->ringing(ring());
    store_.flush();
    transport_->status(410);
    store_.flush();
    EXPECT_TRUE(store_.rows.empty());
    EXPECT_EQ(push_->counters().forgotten, 1U);
    store_for("bob");
    store_.fail = true;
    push_->ringing(ring());
    store_.flush();
    store_.fail = false;
    // The list failed: nothing to send, and counted.
    EXPECT_EQ(push_->counters().store_failures, 1U);
    push_->ringing(ring());
    store_.flush();
    store_.fail = true;
    transport_->status(404);
    store_.flush();
    EXPECT_EQ(push_->counters().store_failures, 2U);
    EXPECT_EQ(store_.rows.size(), 1U);
}

TEST_F(PushTest, LookupsPastTheCapPushToNobody) {
    store_for("bob");
    push_->ringing(ring({user("bob"), user("carol"), user("dave")}));
    EXPECT_EQ(push_->counters().lookups, 2U);
    EXPECT_EQ(push_->counters().lookups_dropped, 1U);
    store_.flush();
    push_->ringing(ring({user("bob")}));
    EXPECT_EQ(push_->counters().lookups, 3U);
}

TEST_F(PushTest, ARingThatEndedWhileTheListWasReadPushesNothing) {
    store_for("bob");
    push_->ringing(ring());
    clock_.advance(45s);
    store_.flush();
    EXPECT_TRUE(transport_->posts.empty());
    EXPECT_EQ(push_->counters().messages, 0U);
}

TEST_F(PushTest, AStoredSubscriptionTheAllowlistNoLongerNamesIsSkipped) {
    store_for("bob", "https://push.example.com/x");
    store_for("bob", std::string(kEndpoint), kOtherDevice);
    push_->ringing(ring());
    store_.flush();
    // The default list does not name push.example.com.
    EXPECT_EQ(push_->counters().skipped, 1U);
    EXPECT_EQ(transport_->posts.size(), 1U);
    // A stored key that is not a point cannot be encrypted to.
    store_.rows[1].subscription.p256dh[64] ^= 1U;
    transport_->posts.clear();
    push_->ringing(ring());
    store_.flush();
    EXPECT_EQ(push_->counters().skipped, 3U);
    EXPECT_TRUE(transport_->posts.empty());
}

TEST_F(PushTest, TheSenderForgetsThePushOnceItIsGone) {
    store_for("bob");
    push_->ringing(ring());
    store_.flush();
    push_.reset();
    transport_->status(410);
    EXPECT_EQ(store_.waiting(), 0U);
    EXPECT_EQ(store_.rows.size(), 1U);
    EXPECT_EQ(sender_->counters().gone, 1U);
}

TEST_F(PushTest, TickRunsTheSendersRetries) {
    store_for("bob");
    push_->ringing(ring());
    store_.flush();
    transport_->status(503);
    EXPECT_EQ(push_->queued(), 1U);
    clock_.advance(1s);
    push_->tick();
    EXPECT_EQ(push_->in_flight(), 1U);
    EXPECT_EQ(push_->sender_counters().retried, 1U);
}

} // namespace

// A call's push, end to end but for the browser: the ring on the room's owner, the callee's
// subscriptions in Postgres, the encrypted message signed with the VAPID key and POSTed over TLS
// by libcurl on the reactor, to a push service in this test that decrypts it with the
// subscription's private key, as the browser would. No external network.
#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"
#include "infra/curl/multi.hpp"
#include "infra/postgres/push_subscriptions.hpp"
#include "infra/webpush/curl_transport.hpp"
#include "infra/webpush/ece.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "fake_push_service.hpp"
#include "postgres_harness.hpp"
#include "push.hpp"
#include "ring.hpp"
#include "support/reactor_harness.hpp"
#include "support/tls_pki.hpp"

#include <algorithm>
#include <cstdlib>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using ulw::test::pump_until;
using ulw::test::ScratchDatabase;

constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
constexpr std::string_view kSubject = "mailto:ops@example.com";

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

// The ring's room plane: this node owns the room, and the notices go nowhere.
class QuietPlane final : public chat::IRingPlane {
public:
    void notify(const core::RoomId& /*room*/,
                std::span<const std::byte> /*notice*/) noexcept override {}
    [[nodiscard]] bool owns(const core::RoomId& /*room*/) const noexcept override { return true; }
};

class NoClients final : public chat::IClientLookup {
public:
    [[nodiscard]] chat::IClient* client(chat::ClientId /*id*/) noexcept override { return nullptr; }
};

// A browser's subscription: its keys, which only the browser holds the private half of.
struct Browser {
    infra::webpush::KeyPair keys{};
    infra::webpush::AuthSecret auth{};
    std::string path;
};

class PushDeliveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        for (auto* b : {&bob_phone_, &bob_laptop_, &gone_}) {
            b->keys = infra::webpush::generate_key_pair().value();
            // Any 16 bytes the browser would pick; a fresh private key's first ones serve.
            const auto other = infra::webpush::generate_key_pair().value();
            std::copy_n(other.private_key.begin(), b->auth.size(), b->auth.begin());
        }
        bob_phone_.path = "/push/bob-phone";
        bob_laptop_.path = "/push/bob-laptop";
        gone_.path = "/push/gone";
        service_.subscribe(bob_phone_.path, bob_phone_.keys.private_key, bob_phone_.auth);
        service_.subscribe(bob_laptop_.path, bob_laptop_.keys.private_key, bob_laptop_.auth);
        service_.gone(gone_.path);
        auto reactor = net::make_reactor(net::ReactorKind::Epoll, clock_, 1024);
        ASSERT_TRUE(reactor);
        reactor_ = std::move(*reactor);
        auto offload = net::OffloadPool::create(*reactor_, 1);
        ASSERT_TRUE(offload);
        offload_ = std::move(*offload);
        auto store = infra::postgres::PgPushSubscriptions::create(*reactor_, *offload_,
                                                                  {.conninfo = db_->conninfo()});
        ASSERT_TRUE(store) << store.error();
        store_ = std::move(*store);
        auto multi = infra::curl::Multi::create(*reactor_, 4);
        ASSERT_TRUE(multi);
        multi_ = std::move(*multi);
        auto key = infra::webpush::VapidKey::from_private(infra::webpush::PrivateKey{0x5a});
        ASSERT_TRUE(key);
        vapid_.emplace(std::move(*key));
        // As chat_server makes it with ULW_DEV_PUSH_ALLOW_PRIVATE and ULW_DEV_PUSH_CA_FILE: the
        // push service is on loopback, with the test's own CA.
        sender_ = std::make_unique<infra::webpush::PushSender>(
            std::make_unique<infra::webpush::CurlPushTransport>(
                *multi_,
                infra::webpush::CurlTransportOptions{
                    .public_only = false, .ca_file = ulw::test::TestPki::shared().ca_file()}),
            *vapid_, clock_, infra::webpush::SenderLimits{.subject = std::string(kSubject)});
        push_ = std::make_unique<chat::Push>(
            chat::PushDeps{.store = *store_,
                           .sender = *sender_,
                           .key = *vapid_,
                           .hosts = *infra::webpush::PushHosts::parse("localhost"),
                           .limits = {},
                           .dev_any_port = true},
            clients_, clock_);
        ringer_ = std::make_unique<chat::Ringer>(plane_, clock_, random_, chat::RingLimits{},
                                                 push_.get());
    }

    void TearDown() override {
        ringer_.reset();
        offload_.reset();
        store_.reset();
        push_.reset();
        sender_.reset();
        multi_.reset();
        reactor_.reset();
    }

    void subscribe(std::string_view who, const Browser& b, std::string_view device) {
        std::optional<core::ports::PushResult<void>> saved;
        store_->save(user(who),
                     core::ports::PushSubscription{.device = *core::DeviceId::parse(device),
                                                   .endpoint = service_.base_url() + b.path,
                                                   .p256dh = b.keys.public_key,
                                                   .auth = b.auth},
                     10, [&saved](core::ports::PushResult<void> r) noexcept { saved = r; });
        ASSERT_TRUE(pump_until(*reactor_, [&] { return saved.has_value(); }));
        ASSERT_TRUE(*saved);
    }

    std::size_t rows() {
        auto conn = db_->session();
        return std::stoul(ulw::test::scalar(conn, "SELECT count(*) FROM push_subscriptions"));
    }

    // Runs the loop, and the server's per-turn work, until `pred` holds.
    template <class Pred> bool run_until(Pred pred) {
        return pump_until(*reactor_, [&] {
            ringer_->tick();
            push_->tick();
            return pred();
        });
    }

    std::unique_ptr<ScratchDatabase> db_;
    os::SystemClock clock_;
    os::SystemRandom random_;
    Browser bob_phone_;
    Browser bob_laptop_;
    Browser gone_;
    ulw::test::FakePushService service_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<net::OffloadPool> offload_;
    std::unique_ptr<infra::postgres::PgPushSubscriptions> store_;
    std::unique_ptr<infra::curl::Multi> multi_;
    std::optional<infra::webpush::VapidKey> vapid_;
    std::unique_ptr<infra::webpush::PushSender> sender_;
    QuietPlane plane_;
    NoClients clients_;
    std::unique_ptr<chat::Push> push_;
    std::unique_ptr<chat::Ringer> ringer_;
};

TEST_F(PushDeliveryTest, ARingReachesEveryDeviceOfTheCalleeEncryptedAndSigned) {
    subscribe("bob", bob_phone_, "01a0eb86-6cca-7dce-84cc-3bb47615f9d1");
    subscribe("bob", bob_laptop_, "01a0eb86-6cca-7dce-84cc-3bb47615f9d2");
    const auto room = *core::RoomId::parse(kRoom);
    const auto call = ringer_->ticketed(room, user("alice"),
                                        std::vector<core::UserId>{user("alice"), user("bob")});
    ASSERT_TRUE(call && *call);
    ASSERT_TRUE(run_until([&] { return sender_->counters().delivered == 2; }));
    const auto delivered = service_.delivered();
    ASSERT_EQ(delivered.size(), 2U);
    const auto expires =
        std::chrono::duration_cast<core::Seconds>(
            (clock_.wall_now() + chat::RingLimits{}.ring_timeout).time_since_epoch())
            .count();
    std::vector<std::string> paths;
    for (const ulw::test::FakePushService::Delivered& d : delivered) {
        EXPECT_TRUE(d.decrypted) << d.path;
        paths.push_back(d.path);
        // Exactly what the callee's sockets hear, and nothing of any message.
        const auto json = core::json::parse(d.plaintext);
        ASSERT_TRUE(json.has_value()) << d.plaintext;
        EXPECT_EQ(json->find("type")->as_string(), "call_ringing");
        EXPECT_EQ(json->find("room")->as_string(), kRoom);
        EXPECT_EQ(json->find("call")->as_string(), (*call)->to_string());
        EXPECT_EQ(json->find("from")->as_string(), "alice");
        const auto at = json->find("expires_at")->as_i64();
        ASSERT_TRUE(at.has_value());
        EXPECT_LE(std::abs(*at - expires), 2);
        EXPECT_EQ(json->as_object()->size(), 5U);
        EXPECT_EQ(d.headers.at("content-encoding"), "aes128gcm");
        EXPECT_EQ(d.headers.at("urgency"), "high");
        const int ttl = std::stoi(d.headers.at("ttl"));
        EXPECT_GE(ttl, 43);
        EXPECT_LE(ttl, 45);
        const std::string& auth = d.headers.at("authorization");
        EXPECT_TRUE(auth.starts_with("vapid t=")) << auth;
        EXPECT_TRUE(auth.ends_with(", k=" + vapid_->public_key()));
        // The token names the push service's own origin as its audience.
        const std::string token = auth.substr(8, auth.find(',') - 8);
        const std::size_t dot = token.find('.');
        const auto claims = core::json::parse(
            infra::auth::decode_base64url(token.substr(dot + 1, token.rfind('.') - dot - 1))
                .value_or(""));
        ASSERT_TRUE(claims.has_value());
        EXPECT_EQ(claims->find("aud")->as_string(),
                  "https://localhost:" + std::to_string(service_.port()));
        EXPECT_EQ(claims->find("sub")->as_string(), kSubject);
    }
    std::ranges::sort(paths);
    EXPECT_EQ(paths, (std::vector<std::string>{"/push/bob-laptop", "/push/bob-phone"}));
    // The caller has no devices rung.
    EXPECT_EQ(push_->counters().lookups, 1U);
}

TEST_F(PushDeliveryTest, ASubscriptionThePushServiceCallsGoneIsDeleted) {
    subscribe("bob", gone_, "01a0eb86-6cca-7dce-84cc-3bb47615f9d3");
    subscribe("bob", bob_phone_, "01a0eb86-6cca-7dce-84cc-3bb47615f9d1");
    ASSERT_EQ(rows(), 2U);
    const auto call = ringer_->ticketed(*core::RoomId::parse(kRoom), user("alice"),
                                        std::vector<core::UserId>{user("alice"), user("bob")});
    ASSERT_TRUE(call && *call);
    ASSERT_TRUE(run_until([&] { return push_->counters().forgotten == 1; }));
    EXPECT_EQ(sender_->counters().gone, 1U);
    EXPECT_EQ(rows(), 1U);
    ASSERT_TRUE(run_until([&] { return sender_->counters().delivered == 1; }));
}

} // namespace

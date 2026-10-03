// The Postgres push subscription store (migrations/0015_push_subscriptions.sql): what it keeps,
// for whom, and what it forgets.
#include "infra/postgres/push_subscriptions.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "postgres_harness.hpp"
#include "support/reactor_harness.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using core::ports::PushResult;
using core::ports::PushSubscription;
using infra::postgres::Params;
using ulw::test::ScratchDatabase;
using List = std::vector<PushSubscription>;

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

core::DeviceId device(int n) {
    return *core::DeviceId::parse("01a0eb86-6cca-7dce-84cc-3bb47615f9" + std::to_string(10 + n));
}

PushSubscription subscription(int n, std::string endpoint = {}) {
    PushSubscription s{.device = device(n),
                       .endpoint = endpoint.empty()
                                       ? "https://fcm.googleapis.com/fcm/send/" + std::to_string(n)
                                       : std::move(endpoint),
                       .p256dh = {},
                       .auth = {}};
    s.p256dh.fill(static_cast<std::uint8_t>(n));
    s.p256dh[0] = 0x04;
    s.auth.fill(static_cast<std::uint8_t>(0x80 + n));
    return s;
}

class PushSubscriptionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        conn_.emplace(db_->session());
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
    }

    void TearDown() override {
        offload_.reset();
        store_.reset();
        reactor_.reset();
    }

    template <class T, class Call> PushResult<T> ask(Call call) {
        std::optional<PushResult<T>> answer;
        call([&answer](PushResult<T> r) noexcept { answer = std::move(r); });
        if (!ulw::test::pump_until(*reactor_, [&] { return answer.has_value(); })) {
            ADD_FAILURE() << "the push store never answered";
            return std::unexpected(core::ports::PushStoreError::Unavailable);
        }
        return std::move(*answer);
    }

    PushResult<void> save(std::string_view who, const PushSubscription& s, std::size_t max = 10) {
        return ask<void>([&](auto done) { store_->save(user(who), s, max, std::move(done)); });
    }
    PushResult<List> list(std::string_view who, std::size_t limit = 10) {
        return ask<List>([&](auto done) { store_->list(user(who), limit, std::move(done)); });
    }
    std::vector<std::string> endpoints(std::string_view who) {
        std::vector<std::string> out;
        for (const PushSubscription& s : list(who).value_or(List{})) {
            out.push_back(s.endpoint);
        }
        return out;
    }

    std::unique_ptr<ScratchDatabase> db_;
    std::optional<infra::postgres::SyncConnection> conn_;
    os::SystemClock clock_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<net::OffloadPool> offload_;
    std::unique_ptr<infra::postgres::PgPushSubscriptions> store_;
};

TEST_F(PushSubscriptionsTest, ASavedSubscriptionComesBackAsItWasSaved) {
    ASSERT_TRUE(save("alice", subscription(1)));
    const auto got = list("alice");
    ASSERT_TRUE(got);
    ASSERT_EQ(got->size(), 1U);
    const PushSubscription& s = got->front();
    EXPECT_EQ(s.device, device(1));
    EXPECT_EQ(s.endpoint, "https://fcm.googleapis.com/fcm/send/1");
    EXPECT_EQ(s.p256dh, subscription(1).p256dh);
    EXPECT_EQ(s.auth, subscription(1).auth);
    EXPECT_TRUE(list("bob")->empty());
}

TEST_F(PushSubscriptionsTest, SavingADeviceAgainReplacesItsSubscription) {
    ASSERT_TRUE(save("alice", subscription(1)));
    PushSubscription renewed = subscription(1, "https://fcm.googleapis.com/fcm/send/renewed");
    renewed.auth.fill(0x11);
    ASSERT_TRUE(save("alice", renewed));
    const auto got = list("alice");
    ASSERT_EQ(got->size(), 1U);
    EXPECT_EQ(got->front().endpoint, "https://fcm.googleapis.com/fcm/send/renewed");
    EXPECT_EQ(got->front().auth, renewed.auth);
}

TEST_F(PushSubscriptionsTest, AnEndpointBelongsToTheLastUserWhoSavedIt) {
    // Another account signed in on the same browser takes its subscription over.
    ASSERT_TRUE(save("alice", subscription(1)));
    ASSERT_TRUE(save("bob", subscription(2, "https://fcm.googleapis.com/fcm/send/1")));
    EXPECT_TRUE(list("alice")->empty());
    EXPECT_EQ(endpoints("bob"),
              (std::vector<std::string>{"https://fcm.googleapis.com/fcm/send/1"}));
}

TEST_F(PushSubscriptionsTest, PastTheCapTheDevicesSavedLongestAgoAreForgotten) {
    for (int n = 1; n <= 4; ++n) {
        ASSERT_TRUE(save("alice", subscription(n), 3));
    }
    EXPECT_EQ(endpoints("alice"),
              (std::vector<std::string>{"https://fcm.googleapis.com/fcm/send/4",
                                        "https://fcm.googleapis.com/fcm/send/3",
                                        "https://fcm.googleapis.com/fcm/send/2"}));
    // Saving device 2 again makes it the newest; the next new one pushes out device 3.
    ASSERT_TRUE(save("alice", subscription(2), 3));
    ASSERT_TRUE(save("alice", subscription(5), 3));
    EXPECT_EQ(endpoints("alice"),
              (std::vector<std::string>{"https://fcm.googleapis.com/fcm/send/5",
                                        "https://fcm.googleapis.com/fcm/send/2",
                                        "https://fcm.googleapis.com/fcm/send/4"}));
    // Another user's devices are not counted.
    ASSERT_TRUE(save("bob", subscription(9), 1));
    EXPECT_EQ(list("alice")->size(), 3U);
    // A list is cut at its limit.
    EXPECT_EQ(list("alice", 2)->size(), 2U);
}

TEST_F(PushSubscriptionsTest, RemoveForgetsOneDeviceAndForgetOneEndpoint) {
    ASSERT_TRUE(save("alice", subscription(1)));
    ASSERT_TRUE(save("alice", subscription(2)));
    ASSERT_TRUE(save("bob", subscription(3)));
    ASSERT_TRUE(
        ask<void>([&](auto done) { store_->remove(user("alice"), device(1), std::move(done)); }));
    // Another user's device of the same id is not theirs to remove.
    ASSERT_TRUE(
        ask<void>([&](auto done) { store_->remove(user("alice"), device(3), std::move(done)); }));
    EXPECT_EQ(endpoints("alice"),
              (std::vector<std::string>{"https://fcm.googleapis.com/fcm/send/2"}));
    EXPECT_EQ(list("bob")->size(), 1U);
    ASSERT_TRUE(ask<void>([&](auto done) {
        store_->forget("https://fcm.googleapis.com/fcm/send/3", std::move(done));
    }));
    EXPECT_TRUE(list("bob")->empty());
    // Forgetting what is not there is done too.
    EXPECT_TRUE(
        ask<void>([&](auto done) { store_->forget("https://nowhere/x", std::move(done)); }));
}

TEST_F(PushSubscriptionsTest, TheTableRefusesRowsChatServerCouldNotHaveWritten) {
    const auto insert = [&](std::string_view endpoint, int p256dh, int auth) {
        return conn_->exec("INSERT INTO push_subscriptions (user_id, device_id, endpoint, p256dh, "
                           "auth) VALUES ('x', gen_random_uuid(), $1, "
                           "decode(repeat('04', $2::integer), 'hex'), "
                           "decode(repeat('00', $3::integer), 'hex'))",
                           Params{}.add_text(endpoint).add_int(p256dh).add_int(auth));
    };
    EXPECT_TRUE(insert("https://a.example/x", 65, 16));
    EXPECT_FALSE(insert("http://a.example/y", 65, 16));
    EXPECT_FALSE(insert("https://a.example/" + std::string(2048, 'a'), 65, 16));
    EXPECT_FALSE(insert("https://a.example/z", 64, 16));
    EXPECT_FALSE(insert("https://a.example/z", 65, 17));
    // The endpoint is unique.
    EXPECT_FALSE(insert("https://a.example/x", 65, 16));
}

TEST_F(PushSubscriptionsTest, AListOfARowItCannotReadIsCorrupt) {
    ASSERT_TRUE(save("alice", subscription(1)));
    ASSERT_TRUE(conn_->exec("ALTER TABLE push_subscriptions DROP CONSTRAINT "
                            "push_subscriptions_auth_length"));
    ASSERT_TRUE(conn_->exec("UPDATE push_subscriptions SET auth = '\\x00'"));
    EXPECT_EQ(list("alice").error(), core::ports::PushStoreError::Corrupt);
}

TEST_F(PushSubscriptionsTest, AStoreThatCannotBeReachedIsUnavailable) {
    offload_.reset();
    store_.reset();
    auto offload = net::OffloadPool::create(*reactor_, 1);
    ASSERT_TRUE(offload);
    offload_ = std::move(*offload);
    // A port nothing listens on, refused at once.
    auto store = infra::postgres::PgPushSubscriptions::create(
        *reactor_, *offload_,
        {.conninfo = "host=127.0.0.1 port=1 dbname=x user=x connect_timeout=1",
         .connections = 1,
         .connect_timeout = core::Millis{500},
         .request_timeout = core::Millis{500}});
    ASSERT_TRUE(store);
    store_ = std::move(*store);
    EXPECT_EQ(save("alice", subscription(1)).error(), core::ports::PushStoreError::Unavailable);
    EXPECT_EQ(list("alice").error(), core::ports::PushStoreError::Unavailable);
}

} // namespace

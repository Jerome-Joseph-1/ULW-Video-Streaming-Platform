#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "key_fetcher.hpp"
#include "support/http_test_server.hpp"
#include "support/reactor_harness.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using ulw::test::HttpTestServer;
using ulw::test::pump_until;
using ulw::test::Reply;
using ulw::test::ServedRequest;

struct Receiver final : infra::auth::IKeySetReceiver {
    std::vector<std::optional<std::string>> results;
    void on_key_set(std::optional<std::string_view> body) noexcept override {
        results.emplace_back(body ? std::optional<std::string>(*body) : std::nullopt);
    }
};

class KeySetFetcherTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock, 1024);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        auto m = infra::curl::Multi::create(*reactor);
        ASSERT_TRUE(m);
        multi = std::move(*m);
        fetcher = std::make_unique<gateway::KeySetFetcher>(*multi);
    }
    void TearDown() override {
        fetcher.reset();
        multi.reset();
        reactor.reset();
    }

    os::SystemClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<gateway::KeySetFetcher> fetcher;
};

HttpTestServer serving(int status, std::string body) {
    return HttpTestServer([status, body = std::move(body)](const ServedRequest&) {
        return Reply{.status = status, .headers = {}, .body = body};
    });
}

TEST_P(KeySetFetcherTest, HandsTheReceiverTheKeySet) {
    auto server = serving(200, R"({"keys":[]})");
    Receiver receiver;
    fetcher->fetch(server.base_url() + "/jwks", receiver);
    ASSERT_TRUE(pump_until(*reactor, [&] { return !receiver.results.empty(); }));
    ASSERT_EQ(receiver.results.size(), 1U);
    EXPECT_EQ(receiver.results[0], R"({"keys":[]})");
}

TEST_P(KeySetFetcherTest, AnythingButOkIsAFailedFetch) {
    auto server = serving(503, "try later");
    Receiver receiver;
    fetcher->fetch(server.base_url() + "/jwks", receiver);
    ASSERT_TRUE(pump_until(*reactor, [&] { return !receiver.results.empty(); }));
    ASSERT_EQ(receiver.results.size(), 1U);
    EXPECT_FALSE(receiver.results[0].has_value());
}

TEST_P(KeySetFetcherTest, AnUnreachableServerIsAFailedFetch) {
    Receiver receiver;
    // Nothing listens on port 1 of the loopback address.
    fetcher->fetch("http://127.0.0.1:1/jwks", receiver);
    ASSERT_TRUE(pump_until(*reactor, [&] { return !receiver.results.empty(); }));
    EXPECT_FALSE(receiver.results[0].has_value());
}

TEST_P(KeySetFetcherTest, ACancelledFetchNeverReachesItsReceiver) {
    auto server = serving(200, R"({"keys":[]})");
    Receiver cancelled;
    Receiver kept;
    fetcher->fetch(server.base_url() + "/a", cancelled);
    fetcher->fetch(server.base_url() + "/b", kept);
    fetcher->cancel(cancelled);
    ASSERT_TRUE(pump_until(*reactor, [&] { return fetcher->in_flight() == 0; }));
    EXPECT_EQ(server.request_count(), 2U);
    EXPECT_EQ(kept.results.size(), 1U);
    EXPECT_TRUE(cancelled.results.empty());
}

TEST_P(KeySetFetcherTest, AFetchAfterCancelGetsOnlyItsOwnResult) {
    auto first = serving(503, "old");
    auto second = serving(200, "new");
    Receiver receiver;
    fetcher->fetch(first.base_url() + "/jwks", receiver);
    fetcher->cancel(receiver);
    fetcher->fetch(second.base_url() + "/jwks", receiver);
    ASSERT_TRUE(pump_until(*reactor, [&] { return fetcher->in_flight() == 0; }));
    EXPECT_EQ(first.request_count(), 1U);
    ASSERT_EQ(receiver.results.size(), 1U);
    EXPECT_EQ(receiver.results[0], "new");
}

INSTANTIATE_TEST_SUITE_P(Reactors, KeySetFetcherTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace

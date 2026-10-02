// chat_server's key set fetcher, a copy of the gateway's (key_fetcher.hpp): whatever the key
// server answers other than 200 and a key set within bounds, the verifier is told the fetch
// failed and handed no body to take keys from.
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "key_fetcher.hpp"
#include "support/http_test_server.hpp"
#include "support/reactor_harness.hpp"

#include <cstddef>
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

class ChatKeySetFetcherTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock, 1024);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        auto m = infra::curl::Multi::create(*reactor);
        ASSERT_TRUE(m);
        multi = std::move(*m);
        fetcher = std::make_unique<chat::KeySetFetcher>(*multi);
    }
    void TearDown() override {
        fetcher.reset();
        multi.reset();
        reactor.reset();
    }

    // The one result `url` gets.
    std::optional<std::string> fetch(const std::string& url) {
        Receiver receiver;
        fetcher->fetch(url, receiver);
        EXPECT_TRUE(pump_until(*reactor, [&] { return !receiver.results.empty(); }));
        EXPECT_TRUE(pump_until(*reactor, [&] { return fetcher->in_flight() == 0; }));
        EXPECT_EQ(receiver.results.size(), 1U);
        return receiver.results.empty() ? std::nullopt : receiver.results.front();
    }

    os::SystemClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<chat::KeySetFetcher> fetcher;
};

HttpTestServer serving(int status, std::string body) {
    return HttpTestServer([status, body = std::move(body)](const ServedRequest&) {
        return Reply{.status = status, .headers = {}, .body = body};
    });
}

TEST_P(ChatKeySetFetcherTest, HandsTheReceiverTheKeySet) {
    auto server = serving(200, R"({"keys":[]})");
    EXPECT_EQ(fetch(server.base_url() + "/jwks"), R"({"keys":[]})");
    ASSERT_EQ(server.request_count(), 1U);
    EXPECT_EQ(server.requests()[0].method, "GET");
    EXPECT_EQ(server.requests()[0].path(), "/jwks");
}

// An error page, a redirect or a cache's stale answer may carry anything, keys included; none
// of it is handed over to be taken for the key set.
TEST_P(ChatKeySetFetcherTest, AnythingButOkIsAFailedFetchWhateverItsBody) {
    for (const int status : {201, 204, 301, 404, 500, 503}) {
        auto server = serving(status, R"({"keys":[{"kty":"OKP","crv":"Ed25519","x":"AAAA"}]})");
        EXPECT_EQ(fetch(server.base_url() + "/jwks"), std::nullopt) << status;
    }
}

TEST_P(ChatKeySetFetcherTest, AKeySetPastSixtyFourKibibytesIsAFailedFetch) {
    constexpr std::size_t kBound = std::size_t{64} * 1024;
    auto server = serving(200, std::string(kBound + 1, ' '));
    EXPECT_EQ(fetch(server.base_url() + "/jwks"), std::nullopt);
    auto at_bound = serving(200, std::string(kBound, ' '));
    const auto body = fetch(at_bound.base_url() + "/jwks");
    ASSERT_TRUE(body);
    EXPECT_EQ(body->size(), kBound);
}

TEST_P(ChatKeySetFetcherTest, AnUnreachableOrUnusableUrlIsAFailedFetch) {
    // Nothing listens on port 1 of the loopback address.
    EXPECT_EQ(fetch("http://127.0.0.1:1/jwks"), std::nullopt);
    EXPECT_EQ(fetch("not a url"), std::nullopt);
}

TEST_P(ChatKeySetFetcherTest, ACancelledFetchNeverReachesItsReceiverAndALaterOneGetsItsOwn) {
    auto first = serving(503, "old");
    auto second = serving(200, "new");
    Receiver cancelled;
    Receiver receiver;
    fetcher->fetch(first.base_url() + "/a", cancelled);
    fetcher->fetch(first.base_url() + "/b", receiver);
    fetcher->cancel(cancelled);
    fetcher->cancel(receiver);
    fetcher->fetch(second.base_url() + "/jwks", receiver);
    ASSERT_TRUE(pump_until(*reactor, [&] { return fetcher->in_flight() == 0; }));
    EXPECT_EQ(first.request_count(), 2U);
    EXPECT_TRUE(cancelled.results.empty());
    ASSERT_EQ(receiver.results.size(), 1U);
    EXPECT_EQ(receiver.results[0], "new");
}

INSTANTIATE_TEST_SUITE_P(Reactors, ChatKeySetFetcherTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace

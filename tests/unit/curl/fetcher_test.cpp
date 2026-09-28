#include "infra/curl/fetcher.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "support/http_test_server.hpp"
#include "support/reactor_harness.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>

namespace {

using infra::curl::FailureKind;
using infra::curl::HttpFetcher;
using infra::curl::Result;
using ulw::test::HttpTestServer;
using ulw::test::pump_until;
using ulw::test::Reply;
using ulw::test::ServedRequest;

class FetcherTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock, 1024);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        auto m = infra::curl::Multi::create(*reactor);
        ASSERT_TRUE(m);
        multi = std::move(*m);
        fetcher = std::make_unique<HttpFetcher>(*multi);
    }
    void TearDown() override {
        fetcher.reset();
        multi.reset();
        reactor.reset();
    }

    os::SystemClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<HttpFetcher> fetcher;
};

HttpTestServer json_server() {
    return HttpTestServer([](const ServedRequest&) {
        return Reply{.status = 200, .headers = {}, .body = R"({"keys":[]})"};
    });
}

TEST_P(FetcherTest, DeliversStatusAndBodyOnTheLoop) {
    auto server = json_server();
    std::optional<Result> got;
    ASSERT_TRUE(fetcher->get(server.base_url() + "/jwks", 1024,
                             [&](Result r) noexcept { got = std::move(r); }));
    EXPECT_FALSE(got.has_value()) << "callback ran inside get()";
    EXPECT_EQ(fetcher->pending(), 1U);
    ASSERT_TRUE(pump_until(*reactor, [&] { return got.has_value(); }));
    ASSERT_TRUE(got->has_value());
    EXPECT_EQ((*got)->status, 200);
    EXPECT_EQ((*got)->body, R"({"keys":[]})");
    EXPECT_EQ(fetcher->pending(), 0U);
}

TEST_P(FetcherTest, OversizedBodyFailsRatherThanArrivingCut) {
    auto server = json_server();
    std::optional<Result> got;
    ASSERT_TRUE(fetcher->get(server.base_url() + "/jwks", 4,
                             [&](Result r) noexcept { got = std::move(r); }));
    ASSERT_TRUE(pump_until(*reactor, [&] { return got.has_value(); }));
    ASSERT_FALSE(got->has_value());
    EXPECT_EQ(got->error().kind, FailureKind::BodyTooLarge);
}

TEST_P(FetcherTest, DestroyingTheFetcherDropsPendingCallbacks) {
    auto server = json_server();
    int calls = 0;
    ASSERT_TRUE(fetcher->get(server.base_url() + "/a", 1024, [&](Result) noexcept { ++calls; }));
    ASSERT_TRUE(fetcher->get(server.base_url() + "/b", 1024, [&](Result) noexcept { ++calls; }));
    fetcher.reset();
    ulw::test::pump_for(*reactor, std::chrono::milliseconds(100));
    EXPECT_EQ(calls, 0);
}

TEST_P(FetcherTest, CallbackMayDestroyTheFetcher) {
    auto server = json_server();
    bool called = false;
    ASSERT_TRUE(fetcher->get(server.base_url() + "/a", 1024, [&](Result) noexcept {
        called = true;
        fetcher.reset();
    }));
    ASSERT_TRUE(pump_until(*reactor, [&] { return called; }));
    EXPECT_EQ(fetcher, nullptr);
}

INSTANTIATE_TEST_SUITE_P(Reactors, FetcherTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace

#include "infra/curl/multi.hpp"
#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"

#include "support/eventually.hpp"
#include "support/http_test_server.hpp"
#include "support/reactor_harness.hpp"
#include "support/stalled_resolver.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using infra::curl::FailureKind;
using infra::curl::Method;
using infra::curl::Request;
using infra::curl::Result;
using infra::curl::Transfer;
using ulw::test::HttpTestServer;
using ulw::test::kKiB;
using ulw::test::kMiB;
using ulw::test::pump_until;
using ulw::test::Reply;
using ulw::test::ServedRequest;

struct Outcome final : infra::curl::ITransferHandler {
    int calls = 0;
    std::optional<Result> result;
    // Destroyed from inside the callback when set, as an owner reacting to completion would.
    std::unique_ptr<Transfer>* owner = nullptr;

    void on_transfer_done(Result r) noexcept override {
        ++calls;
        result = std::move(r);
        if (owner != nullptr) {
            owner->reset();
        }
    }
};

// Hands out whatever the test has made available, and counts the times it had nothing.
struct Trickle final : infra::curl::IBodySource {
    std::vector<std::byte> data;
    std::size_t available = 0;
    std::size_t taken = 0;
    int dry_reads = 0;

    std::size_t read_body(std::span<std::byte> out) noexcept override {
        const std::size_t n = std::min(out.size(), available - taken);
        if (n == 0) {
            ++dry_reads;
            return 0;
        }
        std::ranges::copy(std::span(data).subspan(taken, n), out.begin());
        taken += n;
        return n;
    }
};

std::string text(std::span<const std::byte> bytes) {
    std::string out(bytes.size(), '\0');
    std::ranges::transform(bytes, out.begin(), [](std::byte b) { return static_cast<char>(b); });
    return out;
}

std::uint16_t unused_port() {
    // The listener closes as this returns, so a connect to its port is refused.
    const auto listener = net::listen_tcp({.port = 0, .loopback_only = true, .reuse_port = false});
    if (!listener) {
        return 0;
    }
    return net::local_port(listener->get()).value_or(0);
}

class MultiTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock, 1024);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        auto m = infra::curl::Multi::create(*reactor);
        ASSERT_TRUE(m);
        multi = std::move(*m);
    }
    void TearDown() override {
        multi.reset();
        reactor.reset();
    }

    [[nodiscard]] std::unique_ptr<Transfer> get(const std::string& url, Outcome& outcome,
                                                std::size_t max_body = kKiB) {
        auto t = Transfer::start(
            *multi, Request{.method = Method::Get, .url = url, .headers = {}, .max_body = max_body},
            outcome);
        EXPECT_TRUE(t);
        return t ? std::move(*t) : nullptr;
    }

    bool settle(const Outcome& outcome) {
        return pump_until(*reactor, [&] { return outcome.calls > 0; });
    }

    os::SystemClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
};

TEST_P(MultiTest, GetCompletesOnTheLoopWithStatusHeadersAndBody) {
    const HttpTestServer server([](const ServedRequest&) {
        return Reply{.status = 200, .headers = {{"X-Thing", "  value "}}, .body = "hello"};
    });
    Outcome outcome;
    const auto transfer = get(server.base_url() + "/doc", outcome);
    EXPECT_EQ(outcome.calls, 0) << "completed inside start()";
    ASSERT_TRUE(settle(outcome));
    ASSERT_TRUE(outcome.result->has_value()) << outcome.result->error().detail;
    const auto& response = **outcome.result;
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "hello");
    EXPECT_EQ(response.header("x-thing"), "value");
    EXPECT_EQ(response.header("X-THING"), "value");
    EXPECT_EQ(response.header("x-missing"), std::nullopt);
    EXPECT_EQ(server.requests().at(0).target, "/doc");
}

TEST_P(MultiTest, ErrorStatusArrivesAsAResponseWithItsBody) {
    const HttpTestServer server([](const ServedRequest&) {
        return Reply{.status = 503, .headers = {}, .body = "<Error><Code>SlowDown</Code></Error>"};
    });
    Outcome outcome;
    // A success limit of zero must not swallow the explanation of a failure.
    const auto transfer = get(server.base_url() + "/", outcome, 0);
    ASSERT_TRUE(settle(outcome));
    ASSERT_TRUE(outcome.result->has_value());
    EXPECT_EQ((*outcome.result)->status, 503);
    EXPECT_EQ((*outcome.result)->body, "<Error><Code>SlowDown</Code></Error>");
}

TEST_P(MultiTest, SuccessBodyOverTheLimitFailsInsteadOfArrivingTruncated) {
    const HttpTestServer server([](const ServedRequest&) {
        return Reply{.status = 200, .headers = {}, .body = std::string(100, 'x')};
    });
    Outcome at_limit;
    Outcome over_limit;
    const auto a = get(server.base_url() + "/", at_limit, 100);
    ASSERT_TRUE(settle(at_limit));
    const auto b = get(server.base_url() + "/", over_limit, 99);
    ASSERT_TRUE(settle(over_limit));
    ASSERT_TRUE(at_limit.result->has_value());
    EXPECT_EQ((*at_limit.result)->body.size(), 100U);
    ASSERT_FALSE(over_limit.result->has_value());
    EXPECT_EQ(over_limit.result->error().kind, FailureKind::BodyTooLarge);
}

TEST_P(MultiTest, RefusedConnectionIsAConnectFailure) {
    Outcome outcome;
    const auto transfer = get("http://127.0.0.1:" + std::to_string(unused_port()) + "/", outcome);
    ASSERT_TRUE(settle(outcome));
    ASSERT_FALSE(outcome.result->has_value());
    EXPECT_EQ(outcome.result->error().kind, FailureKind::Connect);
    EXPECT_FALSE(outcome.result->error().detail.empty());
}

TEST_P(MultiTest, RedirectIsReturnedRatherThanFollowed) {
    const HttpTestServer server([](const ServedRequest&) {
        return Reply{.status = 302, .headers = {{"Location", "/elsewhere"}}, .body = {}};
    });
    Outcome outcome;
    const auto transfer = get(server.base_url() + "/start", outcome);
    ASSERT_TRUE(settle(outcome));
    ASSERT_TRUE(outcome.result->has_value());
    EXPECT_EQ((*outcome.result)->status, 302);
    EXPECT_EQ(server.request_count(), 1U);
}

TEST_P(MultiTest, StreamedUploadPausesWhileTheSourceIsDryAndSendsExactlyItsLength) {
    const HttpTestServer server([](const ServedRequest&) {
        return Reply{.status = 200, .headers = {{"ETag", "\"abc\""}}, .body = {}};
    });
    // Over libcurl's threshold for adding Expect: 100-continue.
    constexpr std::size_t kLength = 3 * kMiB;
    Trickle source;
    source.data = ulw::test::pattern(kLength);
    Outcome outcome;
    auto started = Transfer::start_upload(*multi,
                                          Request{.method = Method::Put,
                                                  .url = server.base_url() + "/part",
                                                  .headers = {"x-custom: 1"},
                                                  .max_body = 0},
                                          kLength, source, outcome);
    ASSERT_TRUE(started);
    const auto& transfer = *started;

    ASSERT_TRUE(pump_until(*reactor, [&] { return source.dry_reads > 0; }));
    for (std::size_t step = kMiB; step <= kLength; step += kMiB) {
        const int dry = source.dry_reads;
        source.available = step;
        transfer->resume_body();
        ASSERT_TRUE(pump_until(*reactor, [&] {
            return source.taken == step && (step == kLength || source.dry_reads > dry);
        }));
    }
    ASSERT_TRUE(settle(outcome));
    ASSERT_TRUE(outcome.result->has_value()) << outcome.result->error().detail;
    EXPECT_EQ((*outcome.result)->status, 200);
    EXPECT_EQ((*outcome.result)->header("etag"), "\"abc\"");

    const auto served = server.requests().at(0);
    EXPECT_TRUE(served.complete);
    EXPECT_EQ(served.method, "PUT");
    EXPECT_EQ(served.header("content-length"), std::to_string(kLength));
    EXPECT_EQ(served.header("x-custom"), "1");
    EXPECT_EQ(served.header("expect"), std::nullopt);
    EXPECT_EQ(served.header("transfer-encoding"), std::nullopt);
    EXPECT_TRUE(served.body == text(source.data));
}

TEST_P(MultiTest, AnUploadThePeerStopsReadingFailsAsATimeoutAfterTheStallLimit) {
    // Listening but never accepting: the kernel completes the connection and takes bytes until
    // the buffers on both sides are full, then nothing moves, as with a store that hung.
    const auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
    ASSERT_TRUE(listener);
    const std::uint16_t port = *net::local_port(listener->get());
    // libcurl's stall timer runs on its own clock, not the reactor's, so the limit is made
    // small instead of the clock injected.
    constexpr std::chrono::seconds kStallLimit{1};
    auto stalling = infra::curl::Multi::create(*reactor, 1, kStallLimit);
    ASSERT_TRUE(stalling);
    struct Endless final : infra::curl::IBodySource {
        std::size_t read_body(std::span<std::byte> out) noexcept override { return out.size(); }
    } source;
    Outcome outcome;
    const auto started_at = std::chrono::steady_clock::now();
    auto transfer =
        Transfer::start_upload(**stalling,
                               Request{.method = Method::Put,
                                       .url = "http://127.0.0.1:" + std::to_string(port) + "/part",
                                       .headers = {},
                                       .max_body = 0},
                               std::uint64_t{1} << 40U, source, outcome);
    ASSERT_TRUE(transfer);
    // libcurl judges the rate over its last five seconds of samples, so a stall shows only once
    // they have aged out of that window: the limit, plus five seconds, plus scheduling slack.
    constexpr std::chrono::seconds kBound = kStallLimit + std::chrono::seconds(5 + 4);
    ASSERT_TRUE(pump_until(*reactor, [&] { return outcome.calls > 0; }, kBound));
    ASSERT_FALSE(outcome.result->has_value());
    EXPECT_EQ(outcome.result->error().kind, FailureKind::Timeout) << outcome.result->error().detail;
    EXPECT_GE(std::chrono::steady_clock::now() - started_at, kStallLimit);
    transfer->reset();
    stalling->reset();
}

TEST_P(MultiTest, DestroyingARunningUploadCancelsItWithoutCallingTheHandler) {
    const HttpTestServer server([](const ServedRequest&) { return Reply{}; });
    Trickle source;
    source.data = ulw::test::pattern(kMiB);
    source.available = 256 * kKiB;
    Outcome outcome;
    auto transfer = Transfer::start_upload(*multi,
                                           Request{.method = Method::Put,
                                                   .url = server.base_url() + "/part",
                                                   .headers = {},
                                                   .max_body = 0},
                                           kMiB, source, outcome);
    ASSERT_TRUE(transfer);
    ASSERT_TRUE(
        pump_until(*reactor, [&] { return server.body_bytes_in_progress() == source.available; }));
    transfer->reset();
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.request_count() == 1; }));
    ulw::test::pump_for(*reactor, std::chrono::milliseconds(50));
    EXPECT_EQ(outcome.calls, 0);
    const auto served = server.requests().at(0);
    EXPECT_FALSE(served.complete);
    EXPECT_EQ(served.body.size(), source.available);
}

TEST_P(MultiTest, HandlerMayDestroyItsTransferFromTheCallback) {
    const HttpTestServer server(
        [](const ServedRequest&) { return Reply{.status = 204, .headers = {}, .body = {}}; });
    Outcome outcome;
    auto transfer = get(server.base_url() + "/", outcome);
    outcome.owner = &transfer;
    ASSERT_TRUE(settle(outcome));
    EXPECT_EQ(transfer, nullptr);
    ASSERT_TRUE(outcome.result->has_value());
    EXPECT_EQ((*outcome.result)->status, 204);
}

TEST_P(MultiTest, AStalledLookupHoldsUpNeitherTheLoopNorItsOwnCancellation) {
    const HttpTestServer server([](const ServedRequest& r) {
        return Reply{.status = 200, .headers = {}, .body = r.target};
    });
    Outcome stalled;
    auto lookup = get("http://" + std::string(ulw::test::kStalledDomain) + "/", stalled);
    ASSERT_TRUE(pump_until(*reactor, [] { return ulw::test::stalled_lookups() == 1; }));

    Outcome before;
    const auto first = get(server.base_url() + "/before", before);
    ASSERT_TRUE(settle(before));
    ASSERT_TRUE(before.result->has_value()) << before.result->error().detail;
    EXPECT_EQ((*before.result)->body, "/before");

    // Should cancelling wait for the lookup, as it would for as long as a slow DNS server
    // takes, the watchdog lets the lookup go so that the test fails instead of hanging.
    std::jthread watchdog([](const std::stop_token& stop) {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        if (!wake.wait_for(lock, stop, std::chrono::seconds(5),
                           [&] { return stop.stop_requested(); })) {
            ulw::test::release_stalled_lookups();
        }
    });
    lookup.reset();
    const std::size_t still_stalled = ulw::test::stalled_lookups();
    watchdog.request_stop();
    watchdog.join();
    EXPECT_EQ(still_stalled, 1U) << "cancelling waited for getaddrinfo to return";
    EXPECT_EQ(stalled.calls, 0);

    Outcome after;
    const auto second = get(server.base_url() + "/after", after);
    ASSERT_TRUE(settle(after));
    ASSERT_TRUE(after.result->has_value()) << after.result->error().detail;
    EXPECT_EQ((*after.result)->body, "/after");

    ulw::test::release_stalled_lookups();
    EXPECT_TRUE(ulw::test::eventually([] { return ulw::test::stalled_lookups() == 0; }));
}

TEST_P(MultiTest, TransferWhoseSocketTheReactorRefusesFailsInsteadOfHanging) {
    const HttpTestServer server([](const ServedRequest&) { return Reply{}; });
    // Held open so that every descriptor libcurl gets lies beyond the reactor's table.
    const os::UniqueFd lowest{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    ASSERT_TRUE(lowest);
    auto small = net::make_reactor(GetParam(), clock, static_cast<std::size_t>(lowest.get()));
    ASSERT_TRUE(small);
    auto small_multi = infra::curl::Multi::create(**small);
    ASSERT_TRUE(small_multi);
    Outcome outcome;
    auto transfer = Transfer::start(
        **small_multi,
        Request{
            .method = Method::Get, .url = server.base_url() + "/", .headers = {}, .max_body = 0},
        outcome);
    ASSERT_TRUE(transfer);
    ASSERT_TRUE(pump_until(**small, [&] { return outcome.calls > 0; }));
    EXPECT_EQ(outcome.calls, 1);
    ASSERT_FALSE(outcome.result->has_value());
    EXPECT_EQ(outcome.result->error().kind, FailureKind::Local);
    EXPECT_EQ(outcome.result->error().detail, "socket not watchable");
    transfer->reset();
    small_multi->reset();
}

TEST_P(MultiTest, ConcurrentTransfersEachCompleteExactlyOnceWithTheirOwnResponse) {
    const HttpTestServer server([](const ServedRequest& r) {
        return Reply{.status = 200, .headers = {}, .body = r.target};
    });
    constexpr std::size_t kCount = 40;
    std::vector<Outcome> outcomes(kCount);
    std::vector<std::unique_ptr<Transfer>> transfers;
    transfers.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        transfers.push_back(get(server.base_url() + "/item/" + std::to_string(i), outcomes[i]));
    }
    ASSERT_TRUE(pump_until(*reactor, [&] {
        return std::ranges::all_of(outcomes, [](const Outcome& o) { return o.calls > 0; });
    }));
    for (std::size_t i = 0; i < kCount; ++i) {
        EXPECT_EQ(outcomes[i].calls, 1);
        ASSERT_TRUE(outcomes[i].result->has_value());
        EXPECT_EQ((*outcomes[i].result)->body, "/item/" + std::to_string(i));
    }
}

INSTANTIATE_TEST_SUITE_P(Reactors, MultiTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace

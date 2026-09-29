// The gateway over the S3 store and a scripted peer that stops reading a part's body: a store
// that hangs on a connection it holds must fail the upload, since the gateway leaves a body the
// store holds up to the store (docs/adr/0046).
#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "net/socket.hpp"

#include "gateway_harness.hpp"
#include "support/eventually.hpp"
#include "support/http_client.hpp"
#include "support/reactor_harness.hpp"

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using ulw::test::Backend;
using ulw::test::GatewayOptions;
using ulw::test::GatewayUnderTest;
using ulw::test::HttpClient;
using ulw::test::kMiB;

// Answers the store's create, then takes a part's head and never reads its body: the part's
// connection stays open with nothing moving, as against a store that hung.
class HangingStore {
public:
    HangingStore() {
        listener_ = std::move(*net::listen_tcp({.port = 0, .loopback_only = true}));
        port_ = *net::local_port(listener_.get());
        thread_ = std::jthread([this](const std::stop_token& stop) { serve(stop); });
    }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    [[nodiscard]] int parts_held() const { return parts_held_.load(); }

private:
    void serve(const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            pollfd p{.fd = listener_.get(), .events = POLLIN, .revents = 0};
            // Wakes to look at the stop flag; nothing here waits on time passing.
            if (::poll(&p, 1, 50) != 1) {
                continue;
            }
            os::UniqueFd conn{::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC)};
            if (!conn) {
                continue;
            }
            const std::string head = read_head(conn.get());
            if (head.starts_with("POST ")) {
                const std::string body =
                    R"(<?xml version="1.0" encoding="UTF-8"?><InitiateMultipartUploadResult>)"
                    "<Bucket>ulw-test</Bucket><Key>k</Key><UploadId>u1</UploadId>"
                    "</InitiateMultipartUploadResult>";
                const std::string reply =
                    "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body;
                static_cast<void>(::send(conn.get(), reply.data(), reply.size(), MSG_NOSIGNAL));
            } else {
                held_.push_back(std::move(conn));
                ++parts_held_;
            }
        }
    }

    // Byte by byte, so nothing of a body that follows the head is read.
    static std::string read_head(int fd) {
        std::string head;
        char c = 0;
        while (!head.ends_with("\r\n\r\n") && ::recv(fd, &c, 1, 0) == 1) {
            head.push_back(c);
        }
        return head;
    }

    os::UniqueFd listener_;
    std::uint16_t port_ = 0;
    std::vector<os::UniqueFd> held_;
    std::atomic<int> parts_held_{0};
    // Last: stops before the members it uses go.
    std::jthread thread_;
};

TEST(GatewayStoreStall, AStoreThatStopsReadingAPartFailsTheUploadWith503) {
    const HangingStore store;
    // libcurl's stall timer runs on its own clock, not the reactor's, so the limit is made
    // small instead of the clock injected.
    constexpr std::chrono::seconds kStallLimit{1};
    const GatewayOptions options{.backend = Backend::S3,
                                 .store_stall_limit = kStallLimit,
                                 .store_endpoint = store.base_url()};
    GatewayUnderTest gw(options);
    // Two of the store's 8 MiB parts, the most one PATCH carries: more than the kernel buffers
    // between client, gateway and store hold, so the hang reaches the client.
    constexpr std::size_t kSize = 16 * kMiB;
    const auto data = ulw::test::pattern(kSize);

    HttpClient c(gw.endpoint());
    const std::string create = R"({"filename":"trip.mp4","size_bytes":)" + std::to_string(kSize) +
                               R"(,"content_type":"video/mp4"})";
    const auto created =
        c.request("POST", "/api/v1/uploads", "user.alice", std::as_bytes(std::span(create)));
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201);
    const auto doc = core::json::parse(created->body);
    ASSERT_TRUE(doc);
    const std::string id(*doc->find("upload_id")->as_string());

    const auto started = std::chrono::steady_clock::now();
    timeval tv{.tv_sec = 0, .tv_usec = 200'000};
    ::setsockopt(c.fd(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    ASSERT_TRUE(c.send_raw("PATCH /api/v1/uploads/" + id +
                           " HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer user.alice\r\n"
                           "Upload-Offset: 0\r\nContent-Length: " +
                           std::to_string(kSize) + "\r\n\r\n"));
    std::size_t sent = 0;
    while (sent < kSize) {
        const std::size_t n = c.send_some(std::span(data).subspan(sent));
        if (n == 0) {
            break;
        }
        sent += n;
    }
    // The part hung: the gateway stopped reading, so the client could not send it all.
    EXPECT_LT(sent, kSize);
    EXPECT_TRUE(ulw::test::eventually([&] { return store.parts_held() == 1; }));

    const auto r = c.read_response();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    // libcurl judges the rate over its last five seconds of samples: the stall shows once those
    // have aged out, so within the limit plus five seconds, plus scheduling slack.
    EXPECT_LT(std::chrono::steady_clock::now() - started, kStallLimit + std::chrono::seconds(9));
    EXPECT_TRUE(ulw::test::eventually([&] { return gw.claims() == 0; }));
    // Short waits while the part streamed are observed too; the hang is the one that ended
    // the request, observed when the store's failure did: longer than 2.5 s, inside a minute.
    const std::string m = gw.metrics();
    const auto sample = [&m](std::string_view name) -> std::optional<std::uint64_t> {
        const auto at = m.find("\n" + std::string(name) + " ");
        if (at == std::string::npos) {
            return std::nullopt;
        }
        return core::parse_integer<std::uint64_t>(std::string_view(m).substr(
            at + name.size() + 2, m.find('\n', at + 1) - at - name.size() - 2));
    };
    const auto count = sample("backend_write_stall_seconds_count");
    ASSERT_TRUE(count) << m;
    EXPECT_LT(sample(R"(backend_write_stall_seconds_bucket{le="2.5"})"), count) << m;
    EXPECT_EQ(sample(R"(backend_write_stall_seconds_bucket{le="60"})"), count) << m;
}

} // namespace

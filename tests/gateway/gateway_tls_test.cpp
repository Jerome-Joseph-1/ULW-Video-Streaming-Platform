#include "gateway_harness.hpp"
#include "support/eventually.hpp"
#include "support/http_client.hpp"
#include "support/tls_pki.hpp"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace {

using ulw::test::GatewayOptions;
using ulw::test::GatewayUnderTest;
using ulw::test::HttpClient;
using ulw::test::TestPki;

GatewayOptions tls(GatewayOptions options = {}) {
    options.transport = gateway::Transport::Tls;
    return options;
}

bool metric_is(GatewayUnderTest& gw, std::string_view line) {
    return gw.metrics().find(std::string(line) + "\n") != std::string::npos;
}

TEST(GatewayTls, ASighupServesARenewedCertificateWithoutDroppingConnections) {
    const auto files = TestPki::shared().issue("gateway-reload", "first.localhost");
    GatewayOptions options = tls();
    options.tls_files = files;
    GatewayUnderTest gw(options);
    HttpClient before(gw.endpoint());
    ASSERT_TRUE(before.connected());
    EXPECT_EQ(ulw::test::peer_common_name(before.ssl()), "first.localhost");

    static_cast<void>(TestPki::shared().issue("gateway-reload", "second.localhost"));
    gw.reload_certificate();
    const HttpClient after(gw.endpoint());
    ASSERT_TRUE(after.connected());
    EXPECT_EQ(ulw::test::peer_common_name(after.ssl()), "second.localhost");
    EXPECT_EQ(before.request("GET", "/api/v1/healthz", "")->status, 200);

    // A renewal that left a broken key behind is reported and changes nothing.
    std::filesystem::resize_file(files.private_key, 10);
    gw.reload_certificate();
    EXPECT_EQ(gw.counters().certificate_reload_failures, 1U);
    HttpClient still(gw.endpoint());
    ASSERT_TRUE(still.connected());
    EXPECT_EQ(ulw::test::peer_common_name(still.ssl()), "second.localhost");
    EXPECT_EQ(still.request("GET", "/api/v1/healthz", "")->status, 200);
}

TEST(GatewayTls, AClientThatNeverFinishesItsHandshakeIsDroppedAfterFiveSeconds) {
    GatewayUnderTest gw(tls({.manual_clock = true}));
    // A TCP connection that never sends a ClientHello.
    HttpClient silent({.port = gw.port()});
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.connections() == 1; }));
    EXPECT_TRUE(metric_is(gw, "tls_handshakes_in_flight 1"));
    gw.advance(core::Millis{4'900});
    EXPECT_EQ(gw.connections(), 1U);
    // Before the header timeout, which would also have closed it.
    gw.advance(core::Millis{200});
    EXPECT_TRUE(silent.closed_by_peer());
    EXPECT_TRUE(ulw::test::eventually([&] { return gw.connections() == 0; }));
    EXPECT_TRUE(metric_is(gw, "tls_handshakes_in_flight 0"));
    EXPECT_EQ(gw.counters().timeouts_header, 0U);
}

TEST(GatewayTls, PlaintextSentToTheTlsPortIsNeverAnswered) {
    GatewayUnderTest gw(tls());
    HttpClient plain({.port = gw.port()});
    ASSERT_TRUE(plain.send_raw("GET /api/v1/healthz HTTP/1.1\r\nHost: t\r\n\r\n"));
    EXPECT_FALSE(plain.read_response());
    EXPECT_TRUE(plain.closed_by_peer());
    EXPECT_TRUE(ulw::test::eventually([&] { return gw.connections() == 0; }));
    EXPECT_TRUE(metric_is(gw, "tls_handshakes_in_flight 0"));
}

TEST(GatewayTls, ASilentConnectionIsNotMistakenForAClosedOne) {
    GatewayUnderTest gw(tls());
    HttpClient silent({.port = gw.port()});
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.connections() == 1; }));
    // Well inside the five-second handshake timeout, which runs on the real clock here.
    EXPECT_FALSE(silent.closed_by_peer(std::chrono::milliseconds(200)));
    EXPECT_EQ(gw.connections(), 1U);
}

} // namespace

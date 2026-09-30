#include "codec/ws/handshake.hpp"
#include "http/method.hpp"
#include "http/request.hpp"
#include "http/request_parser.hpp"

#include <cstddef>
#include <expected>
#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using codec::ws::accept_handshake;
using codec::ws::HandshakeError;
using codec::ws::UpgradeResponse;
using Verdict = std::expected<UpgradeResponse, HandshakeError>;

// The opening handshake of RFC 6455 section 1.3.
constexpr std::string_view kRfcRequest = "GET /chat HTTP/1.1\r\n"
                                         "Host: server.example.com\r\n"
                                         "Upgrade: websocket\r\n"
                                         "Connection: Upgrade\r\n"
                                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                         "Origin: http://example.com\r\n"
                                         "Sec-WebSocket-Protocol: chat, superchat\r\n"
                                         "Sec-WebSocket-Version: 13\r\n"
                                         "\r\n";

constexpr std::string_view kRfcResponse = "HTTP/1.1 101 Switching Protocols\r\n"
                                          "Upgrade: websocket\r\n"
                                          "Connection: Upgrade\r\n"
                                          "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                                          "\r\n";

// Judges the head inside the callback, where http::RequestParser's views are valid.
class HandshakeSink final : public http::IRequestSink {
public:
    std::optional<Verdict> verdict;
    bool complete = false;

    http::HeadVerdict on_head(const http::RequestHead& head) noexcept override {
        verdict = accept_handshake(head);
        return http::HeadVerdict::accept();
    }
    http::BodyVerdict on_body(std::span<const std::byte> /*bytes*/) noexcept override {
        return http::BodyVerdict::Continue;
    }
    void on_message_complete() noexcept override { complete = true; }
};

Verdict judge(std::string_view request) {
    HandshakeSink sink;
    http::RequestParser parser{sink};
    const http::ParseResult r = parser.feed(std::as_bytes(std::span{request}));
    EXPECT_EQ(r, http::ParseProgress::MessageComplete);
    EXPECT_TRUE(sink.complete);
    if (!sink.verdict) {
        ADD_FAILURE() << "no head";
        return std::unexpected(HandshakeError::Malformed);
    }
    return *sink.verdict;
}

std::string replace(std::string_view request, std::string_view from, std::string_view to) {
    std::string out{request};
    const std::size_t at = out.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    out.replace(at, from.size(), to);
    return out;
}

TEST(WsHandshake, ComputesTheAcceptKeyOfRfc6455) {
    const auto key = codec::ws::accept_key("dGhlIHNhbXBsZSBub25jZQ==");

    ASSERT_TRUE(key);
    EXPECT_EQ(std::string_view(key->data(), key->size()), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// accept_key() takes any string, not only a key the handshake has already checked.
TEST(WsHandshake, RefusesToComputeAnAcceptKeyForAnOverlongKey) {
    // 28 characters plus the GUID fill the buffer exactly; one more does not fit.
    EXPECT_TRUE(codec::ws::accept_key(std::string(28, 'A')));
    EXPECT_FALSE(codec::ws::accept_key(std::string(29, 'A')));
    EXPECT_FALSE(codec::ws::accept_key(std::string(4096, 'A')));
}

TEST(WsHandshake, AcceptsKeysUsingEachEndOfTheBase64Alphabet) {
    for (const std::string_view key : {
             "AAAAAAAAAAAAAAAAAAAAAA==", // all zero bytes
             "ZZZZZZZZZZZZZZZZZZZZZQ==",
             "aaaaaaaaaaaaaaaaaaaaag==",
             "zzzzzzzzzzzzzzzzzzzzzw==",
             "000000000000000000000A==",
             "999999999999999999999Q==",
             "+++++++++++++++++++++g==",
             "/////////////////////w==",
         }) {
        EXPECT_TRUE(judge(replace(kRfcRequest, "dGhlIHNhbXBsZSBub25jZQ==", key))) << key;
    }
}

TEST(WsHandshake, FindsATokenWithSpaceBeforeItsComma) {
    EXPECT_TRUE(
        judge(replace(kRfcRequest, "Connection: Upgrade", "Connection: Upgrade ,keep-alive")));
    EXPECT_TRUE(judge(replace(kRfcRequest, "Upgrade: websocket", "Upgrade: websocket\t, h2c")));
}

TEST(WsHandshake, AnswersTheOpeningHandshakeOfRfc6455) {
    const Verdict v = judge(kRfcRequest);

    ASSERT_TRUE(v);
    EXPECT_EQ(v->bytes(), kRfcResponse);
}

TEST(WsHandshake, FindsTheTokensInListsAndInAnyCase) {
    const std::string request =
        replace(replace(kRfcRequest, "Connection: Upgrade", "connection: keep-alive,  UPGRADE"),
                "Upgrade: websocket", "upgrade: WebSocket");

    EXPECT_TRUE(judge(request));
}

TEST(WsHandshake, RefusesAMethodOtherThanGet) {
    const std::string request = replace(kRfcRequest, "GET", "POST");

    EXPECT_EQ(judge(request), std::unexpected(HandshakeError::NotGet));
}

TEST(WsHandshake, RefusesHttp10) {
    EXPECT_EQ(judge(replace(kRfcRequest, "HTTP/1.1", "HTTP/1.0")),
              std::unexpected(HandshakeError::Malformed));
}

TEST(WsHandshake, RefusesARequestWithoutHost) {
    // http::RequestParser refuses such a request before any sink sees it; a head built
    // elsewhere is held to the same rule.
    std::vector<http::HeaderField> headers{
        {.name = "Upgrade", .value = "websocket"},
        {.name = "Connection", .value = "Upgrade"},
        {.name = "Sec-WebSocket-Key", .value = "dGhlIHNhbXBsZSBub25jZQ=="},
        {.name = "Sec-WebSocket-Version", .value = "13"},
    };
    http::RequestHead head{.method = http::Method::Get,
                           .target = "/chat",
                           .version_minor = 1,
                           .content_length = 0,
                           .keep_alive = true,
                           .headers = headers};

    EXPECT_EQ(accept_handshake(head), std::unexpected(HandshakeError::Malformed));
    headers.push_back({.name = "Host", .value = "server.example.com"});
    head.headers = headers;
    EXPECT_TRUE(accept_handshake(head));
}

TEST(WsHandshake, RefusesARequestWithABody) {
    const std::string request = replace(kRfcRequest, "\r\n\r\n", "\r\nContent-Length: 2\r\n\r\nhi");

    EXPECT_EQ(judge(request), std::unexpected(HandshakeError::Malformed));
}

TEST(WsHandshake, RefusesAPlainGet) {
    EXPECT_EQ(judge(replace(kRfcRequest, "Upgrade: websocket\r\n", "")),
              std::unexpected(HandshakeError::NotAnUpgrade));
    EXPECT_EQ(judge(replace(kRfcRequest, "Connection: Upgrade", "Connection: keep-alive")),
              std::unexpected(HandshakeError::NotAnUpgrade));
    EXPECT_EQ(judge(replace(kRfcRequest, "Upgrade: websocket", "Upgrade: h2c")),
              std::unexpected(HandshakeError::NotAnUpgrade));
}

TEST(WsHandshake, RefusesAnyVersionButThirteen) {
    EXPECT_EQ(judge(replace(kRfcRequest, "Version: 13", "Version: 8")),
              std::unexpected(HandshakeError::UnsupportedVersion));
    EXPECT_EQ(judge(replace(kRfcRequest, "Sec-WebSocket-Version: 13\r\n", "")),
              std::unexpected(HandshakeError::UnsupportedVersion));
    EXPECT_EQ(judge(replace(kRfcRequest, "Version: 13\r\n",
                            "Version: 13\r\nSec-WebSocket-Version: 13\r\n")),
              std::unexpected(HandshakeError::UnsupportedVersion));
}

TEST(WsHandshake, RefusesAKeyThatIsNotSixteenBytesOfBase64) {
    for (const std::string_view key : {
             "dGhlIHNhbXBsZSBub25jZQ=",  // 23 characters
             "dGhlIHNhbXBsZSBub25jZQ",   // no padding
             "dGhlIHNhbXBsZSBub25j*Q==", // outside the alphabet
             "dGhlIHNhbXBsZSBub25jZSE=", // 17 bytes
             "dGhlIHNhbXBsZSBub25jZR==", // padding bits set
             "",
         }) {
        EXPECT_EQ(judge(replace(kRfcRequest, "dGhlIHNhbXBsZSBub25jZQ==", key)),
                  std::unexpected(HandshakeError::BadKey))
            << key;
    }
    EXPECT_EQ(judge(replace(kRfcRequest, "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n", "")),
              std::unexpected(HandshakeError::BadKey));
}

TEST(WsHandshake, RefusesARepeatedKey) {
    const std::string request =
        replace(kRfcRequest, "Origin:", "Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\nOrigin:");

    EXPECT_EQ(judge(request), std::unexpected(HandshakeError::BadKey));
}

TEST(WsHandshake, NamesTheSupportedVersionWhenRefusingAnother) {
    const std::string_view r = codec::ws::rejection_response(HandshakeError::UnsupportedVersion);

    EXPECT_TRUE(r.starts_with("HTTP/1.1 426 "));
    EXPECT_NE(r.find("\r\nSec-WebSocket-Version: 13\r\n"), std::string_view::npos);
    EXPECT_TRUE(r.ends_with("Connection: close\r\n\r\n"));
    EXPECT_TRUE(codec::ws::rejection_response(HandshakeError::BadKey).starts_with("HTTP/1.1 400 "));
    EXPECT_TRUE(codec::ws::rejection_response(HandshakeError::NotGet).starts_with("HTTP/1.1 405 "));
}

} // namespace

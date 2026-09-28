#include "http/request_parser.hpp"
#include "http/status.hpp"

#include "recording_sink.hpp"

#include <cstddef>
#include <format>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace {

using http::ParseProgress;
using http::RequestParser;
using http::Status;
using ulw::test::bytes_of;
using ulw::test::drive;
using ulw::test::fatal;
using ulw::test::RecordingSink;

constexpr auto kTooLarge = fatal(Status::RequestHeaderFieldsTooLarge);

struct Outcome {
    http::ParseResult result;
    std::size_t heads = 0;
};

Outcome parse(std::string_view input) {
    RecordingSink sink;
    RequestParser parser{sink};
    const http::ParseResult result = drive(parser, input, input.size());
    return {.result = result, .heads = sink.requests().size()};
}

std::string get_with_target_bytes(std::size_t n) {
    return "GET /" + std::string(n - 1, 'a') + " HTTP/1.1\r\nHost: a\r\n\r\n";
}

TEST(RequestLimits, TargetAtTheLimitIsAccepted) {
    RecordingSink sink;
    RequestParser parser{sink};
    ASSERT_EQ(drive(parser, get_with_target_bytes(RequestParser::kMaxTargetBytes), 1000),
              ParseProgress::NeedMore);
    ASSERT_EQ(sink.requests().size(), 1U);
    EXPECT_EQ(sink.requests()[0].target.size(), RequestParser::kMaxTargetBytes);
}

TEST(RequestLimits, TargetOverTheLimitIsRejected) {
    const Outcome o = parse(get_with_target_bytes(RequestParser::kMaxTargetBytes + 1));
    EXPECT_EQ(o.result, kTooLarge);
    EXPECT_EQ(o.heads, 0U);
}

TEST(RequestLimits, OversizedHeadersFailBeforeTheHeadIsComplete) {
    RecordingSink sink;
    RequestParser parser{sink};
    // No blank line: the head never completes, so only an in-flight check can catch this.
    const std::string unfinished =
        "GET / HTTP/1.1\r\nX-Fill: " + std::string(RequestParser::kMaxHeaderBytes, 'a');

    EXPECT_EQ(parser.feed(bytes_of(unfinished)), kTooLarge);
    EXPECT_TRUE(sink.requests().empty());
}

TEST(RequestLimits, HeaderBudgetIsExactAcrossFragments) {
    RecordingSink sink;
    RequestParser parser{sink};
    constexpr std::string_view kName = "X-Fill";
    const std::size_t fits = RequestParser::kMaxHeaderBytes - kName.size();
    ASSERT_EQ(parser.feed(bytes_of("GET / HTTP/1.1\r\nX-Fill: ")), ParseProgress::NeedMore);

    for (std::size_t i = 0; i < fits; ++i) {
        ASSERT_EQ(parser.feed(bytes_of("a")), ParseProgress::NeedMore) << i;
    }
    EXPECT_EQ(parser.feed(bytes_of("a")), kTooLarge);
}

TEST(RequestLimits, HeadersFillingTheBudgetExactlyAreAccepted) {
    RecordingSink sink;
    RequestParser parser{sink};
    // Only names and values count: "Host" and "a" take 5 bytes, "X-Fill" 6.
    const std::string value(RequestParser::kMaxHeaderBytes - 5 - 6, 'a');

    ASSERT_EQ(drive(parser, "GET / HTTP/1.1\r\nHost: a\r\nX-Fill: " + value + "\r\n\r\n", 4096),
              ParseProgress::NeedMore);
    ASSERT_EQ(sink.requests().size(), 1U);
    EXPECT_EQ(sink.requests()[0].headers.back().second, value);
}

// Host is one of the `count`.
std::string get_with_headers(std::size_t count) {
    std::string request = "GET / HTTP/1.1\r\nHost: a\r\n";
    for (std::size_t i = 1; i < count; ++i) {
        request += std::format("H{}: v\r\n", i);
    }
    return request + "\r\n";
}

TEST(RequestLimits, HeaderCountAtTheLimitIsAccepted) {
    const Outcome o = parse(get_with_headers(RequestParser::kMaxHeaderCount));
    EXPECT_EQ(o.result, ParseProgress::NeedMore);
    EXPECT_EQ(o.heads, 1U);
}

TEST(RequestLimits, OneHeaderOverTheCountIsRejected) {
    const Outcome o = parse(get_with_headers(RequestParser::kMaxHeaderCount + 1));
    EXPECT_EQ(o.result, kTooLarge);
    EXPECT_EQ(o.heads, 0U);
}

// A head of exactly `size` bytes whose padding before a value llhttp skips without storing.
std::string get_padded_to(std::size_t size) {
    constexpr std::string_view kBefore = "GET / HTTP/1.1\r\nHost: a\r\nX-Pad:";
    constexpr std::string_view kAfter = "v\r\n\r\n";
    return std::string{kBefore} + std::string(size - kBefore.size() - kAfter.size(), ' ') +
           std::string{kAfter};
}

TEST(RequestLimits, HeadFillingItsBudgetExactlyIsAccepted) {
    const std::string input = get_padded_to(RequestParser::kMaxHeadBytes);
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{4096}, input.size()}) {
        RecordingSink sink;
        RequestParser parser{sink};
        EXPECT_EQ(drive(parser, input, chunk), ParseProgress::NeedMore) << chunk;
        EXPECT_EQ(sink.requests().size(), 1U) << chunk;
    }
}

TEST(RequestLimits, PaddingBeforeAValueCountsAgainstTheHeadBudget) {
    const std::string input = get_padded_to(RequestParser::kMaxHeadBytes + 1);
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{4096}, input.size()}) {
        RecordingSink sink;
        RequestParser parser{sink};
        EXPECT_EQ(drive(parser, input, chunk), kTooLarge) << chunk;
        EXPECT_TRUE(sink.requests().empty()) << chunk;
    }
}

TEST(RequestLimits, BlankLinesBeforeTheRequestLineCountAgainstTheHeadBudget) {
    std::string input;
    while (input.size() < RequestParser::kMaxHeadBytes) {
        input += "\r\n";
    }
    input += "GET / HTTP/1.1\r\nHost: a\r\n\r\n";

    for (const std::size_t chunk : {std::size_t{1}, input.size()}) {
        RecordingSink sink;
        RequestParser parser{sink};
        EXPECT_EQ(drive(parser, input, chunk), fatal(Status::BadRequest)) << chunk;
        EXPECT_TRUE(sink.requests().empty()) << chunk;
    }
}

std::string patch_with_length(std::string_view length) {
    return std::format("PATCH /u HTTP/1.1\r\nHost: a\r\nContent-Length: {}\r\n\r\n", length);
}

TEST(RequestLimits, ContentLengthAtTheLimitIsAccepted) {
    RecordingSink sink;
    RequestParser parser{sink};
    const std::string head = patch_with_length(std::to_string(RequestParser::kMaxContentLength));

    EXPECT_EQ(parser.feed(bytes_of(head)), ParseProgress::NeedMore);
    ASSERT_EQ(sink.requests().size(), 1U);
    EXPECT_EQ(sink.requests()[0].content_length, RequestParser::kMaxContentLength);
}

TEST(RequestLimits, ContentLengthOverTheLimitIsRejectedBeforeTheHead) {
    const Outcome o =
        parse(patch_with_length(std::to_string(RequestParser::kMaxContentLength + 1)));
    EXPECT_EQ(o.result, fatal(Status::ContentTooLarge));
    EXPECT_EQ(o.heads, 0U);
}

TEST(RequestLimits, MalformedContentLengthIsABadRequest) {
    for (const std::string_view length :
         {"12a", "-1", "+5", "0x10", "1 2", "1,1", "", "99999999999999999999999"}) {
        const Outcome o = parse(patch_with_length(length));
        EXPECT_EQ(o.result, fatal(Status::BadRequest)) << length;
        EXPECT_EQ(o.heads, 0U) << length;
    }
}

TEST(RequestLimits, DuplicateContentLengthIsABadRequest) {
    const Outcome o = parse("PATCH /u HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx");
    EXPECT_EQ(o.result, fatal(Status::BadRequest));
    EXPECT_EQ(o.heads, 0U);
}

TEST(RequestLimits, VersionsOtherThanHttp1AreNotSupported) {
    for (const std::string_view request :
         {"GET / HTTP/1.2\r\n\r\n", "GET / HTTP/2.0\r\n\r\n", "GET / HTTP/0.9\r\n\r\n",
          "GET / HTTP/9.9\r\n\r\n", "GET /\r\n\r\n"}) {
        const Outcome o = parse(request);
        EXPECT_EQ(o.result, fatal(Status::HttpVersionNotSupported)) << request;
        EXPECT_EQ(o.heads, 0U) << request;
    }
}

// llhttp reports these with the same error code as a well-formed version it does not know.
TEST(RequestLimits, MalformedVersionIsABadRequest) {
    for (const std::string_view request :
         {"GET / HTTP/1.1\nHost: a\r\n\r\n", "GET / HTTP/1.1x\r\nHost: a\r\n\r\n",
          "GET / HTTP/1.10\r\nHost: a\r\n\r\n", "GET / HTTP/1.x\r\nHost: a\r\n\r\n",
          "GET / HTTP/x.1\r\nHost: a\r\n\r\n", "GET / HTTP/11\r\nHost: a\r\n\r\n"}) {
        const Outcome o = parse(request);
        EXPECT_EQ(o.result, fatal(Status::BadRequest)) << request;
        EXPECT_EQ(o.heads, 0U) << request;
    }
}

TEST(RequestLimits, GarbledProtocolNameIsABadRequest) {
    const Outcome o = parse("GET / HTTX/1.1\r\n\r\n");
    EXPECT_EQ(o.result, fatal(Status::BadRequest));
}

} // namespace

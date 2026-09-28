#include "http/method.hpp"
#include "http/request_parser.hpp"
#include "http/status.hpp"

#include "recording_sink.hpp"

#include <array>
#include <cstddef>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using http::Method;
using http::ParseProgress;
using http::RequestParser;
using http::Status;
using ulw::test::bytes_of;
using ulw::test::drive;
using ulw::test::fatal;
using ulw::test::Headers;
using ulw::test::RecordedRequest;
using ulw::test::RecordingSink;
using ulw::test::recoverable;

constexpr std::string_view kGet = "GET /api/v1/videos/v1/master.m3u8?t=9 HTTP/1.1\r\n"
                                  "Host: cdn.example\r\n"
                                  "Accept: */*\r\n"
                                  "\r\n";

constexpr std::string_view kPatch = "PATCH /api/v1/uploads/u1 HTTP/1.1\r\n"
                                    "Host: api.example\r\n"
                                    "Upload-Offset: 0\r\n"
                                    "Content-Length: 11\r\n"
                                    "\r\n"
                                    "hello world";

std::vector<RecordedRequest> parse_in_chunks(std::string_view input, std::size_t chunk) {
    RecordingSink sink;
    RequestParser parser{sink};
    EXPECT_EQ(drive(parser, input, chunk), ParseProgress::NeedMore);
    return sink.requests();
}

TEST(RequestParser, DeliversTheHeadAndBodyOfARequest) {
    const auto requests = parse_in_chunks(kPatch, kPatch.size());

    ASSERT_EQ(requests.size(), 1U);
    const RecordedRequest& r = requests[0];
    EXPECT_EQ(r.method, Method::Patch);
    EXPECT_EQ(r.target, "/api/v1/uploads/u1");
    EXPECT_EQ(r.version_minor, 1);
    EXPECT_EQ(r.content_length, 11U);
    EXPECT_TRUE(r.keep_alive);
    EXPECT_EQ(r.headers,
              (Headers{{"Host", "api.example"}, {"Upload-Offset", "0"}, {"Content-Length", "11"}}));
    EXPECT_EQ(r.body, "hello world");
    EXPECT_TRUE(r.complete);
}

TEST(RequestParser, ByteAtATimeMatchesASingleWrite) {
    const std::string get_then_patch = std::string{kGet} + std::string{kPatch};
    const std::string patch_then_get = std::string{kPatch} + std::string{kGet};
    struct Case {
        std::string_view input;
        std::size_t requests;
    };
    const std::array cases{Case{.input = kGet, .requests = 1}, Case{.input = kPatch, .requests = 1},
                           Case{.input = get_then_patch, .requests = 2},
                           Case{.input = patch_then_get, .requests = 2}};

    for (const Case& c : cases) {
        const auto whole = parse_in_chunks(c.input, c.input.size());
        const auto bytewise = parse_in_chunks(c.input, 1);
        ASSERT_EQ(whole.size(), c.requests) << c.input;
        EXPECT_EQ(bytewise, whole) << c.input;
        EXPECT_TRUE(whole.back().complete);
    }
}

TEST(RequestParser, StopsAfterEachRequestUntilReset) {
    RecordingSink sink;
    RequestParser parser{sink};
    const std::string pipelined = std::string{kGet} + std::string{kPatch};

    EXPECT_EQ(parser.feed(bytes_of(pipelined)), ParseProgress::MessageComplete);
    ASSERT_EQ(sink.requests().size(), 1U);
    EXPECT_EQ(parser.resume(), ParseProgress::Paused);
    EXPECT_EQ(sink.requests().size(), 1U);

    parser.reset_for_next_request();
    EXPECT_EQ(parser.resume(), ParseProgress::MessageComplete);
    ASSERT_EQ(sink.requests().size(), 2U);
    EXPECT_EQ(sink.requests()[1].body, "hello world");
    parser.reset_for_next_request();
    EXPECT_EQ(parser.resume(), ParseProgress::NeedMore);
}

TEST(RequestParser, BytesFedWhileStoppedArePreservedInOrder) {
    RecordingSink sink;
    RequestParser parser{sink};
    ASSERT_EQ(parser.feed(bytes_of(kGet)), ParseProgress::MessageComplete);

    std::vector<http::ParseResult> held;
    for (const std::string_view piece :
         {kPatch.substr(0, 20), kPatch.substr(20, 40), kPatch.substr(60)}) {
        held.push_back(parser.feed(bytes_of(piece)));
    }
    EXPECT_EQ(held, std::vector<http::ParseResult>(3, ParseProgress::Paused));
    EXPECT_EQ(sink.requests().size(), 1U);

    parser.reset_for_next_request();
    EXPECT_EQ(parser.resume(), ParseProgress::MessageComplete);
    ASSERT_EQ(sink.requests().size(), 2U);
    EXPECT_EQ(sink.requests()[1], parse_in_chunks(kPatch, kPatch.size()).front());
}

TEST(RequestParser, BytesHeldWhileStoppedAreBounded) {
    RecordingSink sink;
    RequestParser parser{sink};
    ASSERT_EQ(parser.feed(bytes_of(kGet)), ParseProgress::MessageComplete);

    const std::string filler(RequestParser::kMaxRetainedBytes, 'x');
    EXPECT_EQ(parser.feed(bytes_of(filler)), ParseProgress::Paused);
    EXPECT_EQ(parser.feed(bytes_of("x")), fatal(Status::ContentTooLarge));
}

TEST(RequestParser, HeaderLookupIgnoresCaseAndSurroundingWhitespace) {
    RecordingSink sink;
    sink.look_up({"x-request-id", "X-EMPTY", "host", "absent"});
    RequestParser parser{sink};

    ASSERT_EQ(drive(parser,
                    "GET / HTTP/1.1\r\nX-Request-Id: \t r-42 \t \r\nX-Empty:\r\nHost: a\r\n\r\n",
                    1),
              ParseProgress::NeedMore);

    ASSERT_EQ(sink.requests().size(), 1U);
    const std::vector<std::optional<std::string>> expected{"r-42", "", "a", std::nullopt};
    EXPECT_EQ(sink.requests()[0].found, expected);
}

TEST(RequestParser, ResetForgetsThePreviousRequest) {
    RecordingSink sink;
    sink.look_up({"authorization"});
    RequestParser parser{sink};
    const std::string_view first = "PATCH /a HTTP/1.1\r\n"
                                   "Authorization: Bearer secret\r\n"
                                   "Content-Length: 3\r\n"
                                   "\r\n"
                                   "abc";
    const std::string_view second = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";

    ASSERT_EQ(drive(parser, std::string{first} + std::string{second}, 4096),
              ParseProgress::NeedMore);

    ASSERT_EQ(sink.requests().size(), 2U);
    EXPECT_EQ(sink.requests()[0].found.front(), "Bearer secret");
    const RecordedRequest& r = sink.requests()[1];
    EXPECT_EQ(r.found.front(), std::nullopt);
    EXPECT_EQ(r.target, "/b");
    EXPECT_EQ(r.headers, (Headers{{"Host", "x"}}));
    EXPECT_EQ(r.content_length, 0U);
    EXPECT_EQ(r.body, "");
}

TEST(RequestParser, MethodsOutsideTheRoutedSetAreOther) {
    const auto requests = parse_in_chunks("PROPFIND /a HTTP/1.1\r\n\r\n"
                                          "OPTIONS * HTTP/1.1\r\n\r\n"
                                          "DELETE /b HTTP/1.1\r\n\r\n",
                                          7);
    ASSERT_EQ(requests.size(), 3U);
    EXPECT_EQ(requests[0].method, Method::Other);
    EXPECT_EQ(requests[1].method, Method::Options);
    EXPECT_EQ(requests[1].target, "*");
    EXPECT_EQ(requests[2].method, Method::Delete);
}

TEST(RequestParser, Http10IsPersistentOnlyWhenAsked) {
    const auto requests = parse_in_chunks("GET /a HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"
                                          "GET /b HTTP/1.0\r\n\r\n",
                                          4096);
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[0].version_minor, 0);
    EXPECT_TRUE(requests[0].keep_alive);
    EXPECT_FALSE(requests[1].keep_alive);
}

TEST(RequestParser, NothingMayFollowARequestThatClosesTheConnection) {
    RecordingSink sink;
    RequestParser parser{sink};
    const std::string_view closing = "GET / HTTP/1.1\r\nConnection: close\r\n\r\n";

    ASSERT_EQ(parser.feed(bytes_of(closing)), ParseProgress::MessageComplete);
    EXPECT_FALSE(sink.requests().back().keep_alive);
    parser.reset_for_next_request();
    EXPECT_EQ(parser.resume(), ParseProgress::NeedMore);
    EXPECT_EQ(parser.feed(bytes_of(kGet)), fatal(Status::BadRequest));
    EXPECT_EQ(sink.requests().size(), 1U);
}

TEST(RequestParser, PipelinedBytesAfterAClosingRequestAreRejected) {
    RecordingSink sink;
    RequestParser parser{sink};
    const std::string input = "GET / HTTP/1.1\r\nConnection: close\r\n\r\n" + std::string{kGet};

    ASSERT_EQ(parser.feed(bytes_of(input)), ParseProgress::MessageComplete);
    parser.reset_for_next_request();
    EXPECT_EQ(parser.resume(), fatal(Status::BadRequest));
    EXPECT_EQ(sink.requests().size(), 1U);
}

TEST(RequestParser, MalformedInputFailsForGood) {
    RecordingSink sink;
    RequestParser parser{sink};
    const auto expected = fatal(Status::BadRequest);

    EXPECT_EQ(parser.feed(bytes_of("GET / HTTP/1.1\r\nNo-Colon\r\n\r\n")), expected);
    EXPECT_EQ(parser.feed(bytes_of(kGet)), expected);
    parser.reset_for_next_request();
    EXPECT_EQ(parser.resume(), expected);
    EXPECT_TRUE(sink.requests().empty());
}

TEST(RequestParser, RejectedBodilessRequestLeavesTheConnectionUsable) {
    RecordingSink sink;
    sink.reject("/nope", Status::NotFound);
    RequestParser parser{sink};
    const std::string input = "GET /nope HTTP/1.1\r\n\r\n" + std::string{kPatch};

    EXPECT_EQ(parser.feed(bytes_of(input)), recoverable(Status::NotFound));
    parser.reset_for_next_request();
    EXPECT_EQ(parser.resume(), ParseProgress::MessageComplete);

    ASSERT_EQ(sink.requests().size(), 2U);
    EXPECT_FALSE(sink.requests()[0].complete);
    EXPECT_EQ(sink.requests()[1].body, "hello world");
}

TEST(RequestParser, RejectedRequestWithABodyEndsTheConnection) {
    RecordingSink sink;
    sink.reject("/api/v1/uploads/u1", Status::NotFound);
    RequestParser parser{sink};
    const auto expected = fatal(Status::NotFound);

    EXPECT_EQ(parser.feed(bytes_of(kPatch)), expected);
    parser.reset_for_next_request();
    EXPECT_EQ(parser.resume(), expected);
    ASSERT_EQ(sink.requests().size(), 1U);
    EXPECT_EQ(sink.requests()[0].body, "");
}

TEST(RequestParser, RejectedRequestOnAClosingConnectionEndsIt) {
    RecordingSink sink;
    sink.reject("/nope", Status::NotFound);
    RequestParser parser{sink};

    EXPECT_EQ(parser.feed(bytes_of("GET /nope HTTP/1.0\r\n\r\n")), fatal(Status::NotFound));
}

} // namespace

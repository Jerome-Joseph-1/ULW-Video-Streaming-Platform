#include "http/request_parser.hpp"
#include "http/status.hpp"

#include "recording_sink.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace {

using http::RequestParser;
using http::Status;
using ulw::test::bytes_of;
using ulw::test::drive;
using ulw::test::fatal;
using ulw::test::RecordingSink;

struct Outcome {
    http::ParseResult result;
    std::size_t heads = 0;
};

Outcome parse(std::string_view input) {
    RecordingSink sink;
    RequestParser parser{sink};
    const http::ParseResult result = parser.feed(bytes_of(input));
    return {.result = result, .heads = sink.requests().size()};
}

TEST(RequestFraming, ChunkedUploadIsRefusedAsLengthRequired) {
    const Outcome o = parse("PATCH /api/v1/uploads/u1 HTTP/1.1\r\n"
                            "Host: a\r\n"
                            "Transfer-Encoding: chunked\r\n"
                            "\r\n"
                            "5\r\nhello\r\n0\r\n\r\n");
    EXPECT_EQ(o.result, fatal(Status::LengthRequired));
    EXPECT_EQ(o.heads, 0U);
}

TEST(RequestFraming, AnyTransferCodingOnABodyMethodIsRefused) {
    for (const std::string_view request :
         {"POST /api/v1/uploads HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n",
          "PUT /a HTTP/1.1\r\nHost: a\r\ntransfer-ENCODING: identity\r\n\r\n",
          "PATCH /a HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: "
          "chunked\r\n\r\n"}) {
        const Outcome o = parse(request);
        EXPECT_EQ(o.result, fatal(Status::LengthRequired)) << request;
        EXPECT_EQ(o.heads, 0U) << request;
    }
}

TEST(RequestFraming, TransferCodingOnAMethodWithoutABodyIsABadRequest) {
    const Outcome o =
        parse("GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT_EQ(o.result, fatal(Status::BadRequest));
    EXPECT_EQ(o.heads, 0U);
}

TEST(RequestFraming, TransferCodingOnEveryOtherMethodIsABadRequest) {
    for (const std::string_view method : {"GET", "HEAD", "DELETE", "OPTIONS", "TRACE"}) {
        const Outcome o = parse(std::format(
            "{} / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", method));
        EXPECT_EQ(o.result, fatal(Status::BadRequest)) << method;
        EXPECT_EQ(o.heads, 0U) << method;
    }
}

TEST(RequestFraming, ContentLengthWithTransferEncodingIsABadRequestInEitherOrder) {
    for (const std::string_view request :
         {"PATCH /a HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n"
          "0\r\n\r\n",
          "PATCH /a HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n"
          "0\r\n\r\n"}) {
        const Outcome o = parse(request);
        EXPECT_EQ(o.result, fatal(Status::BadRequest)) << request;
        EXPECT_EQ(o.heads, 0U) << request;
    }
}

TEST(RequestFraming, ChunkedAsANonFinalCodingIsABadRequest) {
    const Outcome o =
        parse("POST /a HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked, gzip\r\n\r\n");
    EXPECT_EQ(o.result, fatal(Status::BadRequest));
    EXPECT_EQ(o.heads, 0U);
}

TEST(RequestFraming, ConnectIsRefusedSoItsBodyCannotPassForARequest) {
    const std::string smuggled = "GET /admin HTTP/1.1\r\nHost: a\r\n\r\n";
    const std::string with_body =
        std::format("CONNECT a:443 HTTP/1.1\r\nHost: a:443\r\nContent-Length: {}\r\n\r\n{}",
                    smuggled.size(), smuggled);
    const std::array<std::string_view, 2> requests{with_body,
                                                   "CONNECT a:443 HTTP/1.1\r\nHost: a:443\r\n\r\n"};
    for (const std::string_view request : requests) {
        RecordingSink sink;
        RequestParser parser{sink};
        EXPECT_EQ(drive(parser, request, request.size()), fatal(Status::NotImplemented)) << request;
        EXPECT_EQ(sink.requests().size(), 0U) << request;
    }
}

TEST(RequestFraming, SimilarlyNamedFieldIsNotATransferCoding) {
    RecordingSink sink;
    RequestParser parser{sink};
    const std::string_view input = "PATCH /b HTTP/1.1\r\nHost: a\r\nTransfer-Encoding-Hint: "
                                   "chunked\r\nContent-Length: 2\r\n\r\nok";

    EXPECT_EQ(parser.feed(bytes_of(input)), http::ParseProgress::MessageComplete);
    ASSERT_EQ(sink.requests().size(), 1U);
    EXPECT_EQ(sink.requests()[0].body, "ok");
}

// A fold or a space before the colon is where front ends and origins disagree on which fields
// a request has (RFC 9112 sections 5.1 and 5.2); a field this parser would read differently
// from a proxy in front of it is a request smuggled past that proxy.
TEST(RequestFraming, ObsoleteLineFoldingIsABadRequest) {
    for (const std::string_view request :
         {"PATCH /a HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\nTransfer-Encoding:\r\n chunked\r\n"
          "\r\nok",
          "PATCH /a HTTP/1.1\r\nHost: a\r\nX-Note: one\r\n\ttwo\r\nContent-Length: 2\r\n\r\nok",
          "PATCH /a HTTP/1.1\r\nHost: a\r\nContent-Length:\r\n 2\r\n\r\nok"}) {
        const Outcome o = parse(request);
        EXPECT_EQ(o.result, fatal(Status::BadRequest)) << request;
        EXPECT_EQ(o.heads, 0U) << request;
    }
}

TEST(RequestFraming, WhitespaceBeforeTheColonIsABadRequest) {
    for (const std::string_view request :
         {"PATCH /a HTTP/1.1\r\nHost: a\r\nTransfer-Encoding : chunked\r\nContent-Length: 5\r\n\r\n"
          "0\r\n\r\n",
          "PATCH /a HTTP/1.1\r\nHost: a\r\nContent-Length : 2\r\n\r\nok",
          "PATCH /a HTTP/1.1\r\nHost: a\r\nContent-Length\t: 2\r\n\r\nok"}) {
        const Outcome o = parse(request);
        EXPECT_EQ(o.result, fatal(Status::BadRequest)) << request;
        EXPECT_EQ(o.heads, 0U) << request;
    }
}

TEST(RequestFraming, ConflictingContentLengthsAreABadRequest) {
    for (const std::string_view request :
         {"PATCH /a HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\nContent-Length: 7\r\n\r\nokGET / ",
          "PATCH /a HTTP/1.1\r\nHost: a\r\nContent-Length: 2, 7\r\n\r\nokGET / "}) {
        const Outcome o = parse(request);
        EXPECT_EQ(o.result, fatal(Status::BadRequest)) << request;
        EXPECT_EQ(o.heads, 0U) << request;
    }
}

// A bare LF ends a line for some parsers and not for others, which is enough to hide a field.
TEST(RequestFraming, ALineEndedByABareLineFeedIsABadRequest) {
    for (const std::string_view request :
         {"PATCH /a HTTP/1.1\r\nHost: a\nContent-Length: 2\r\n\r\nok",
          "PATCH /a HTTP/1.1\r\nHost: a\r\nX-Note: a\nTransfer-Encoding: chunked\r\n"
          "Content-Length: 2\r\n\r\nok"}) {
        const Outcome o = parse(request);
        EXPECT_EQ(o.result, fatal(Status::BadRequest)) << request;
        EXPECT_EQ(o.heads, 0U) << request;
    }
}

} // namespace

#include "http/request_parser.hpp"
#include "http/status.hpp"

#include "recording_sink.hpp"

#include <cstddef>
#include <gtest/gtest.h>
#include <string_view>

namespace {

using http::RequestParser;
using http::Status;
using ulw::test::bytes_of;
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
                            "Transfer-Encoding: chunked\r\n"
                            "\r\n"
                            "5\r\nhello\r\n0\r\n\r\n");
    EXPECT_EQ(o.result, fatal(Status::LengthRequired));
    EXPECT_EQ(o.heads, 0U);
}

TEST(RequestFraming, AnyTransferCodingOnABodyMethodIsRefused) {
    for (const std::string_view request :
         {"POST /api/v1/uploads HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n",
          "PUT /a HTTP/1.1\r\ntransfer-ENCODING: identity\r\n\r\n",
          "PATCH /a HTTP/1.1\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n"}) {
        const Outcome o = parse(request);
        EXPECT_EQ(o.result, fatal(Status::LengthRequired)) << request;
        EXPECT_EQ(o.heads, 0U) << request;
    }
}

TEST(RequestFraming, TransferCodingOnAMethodWithoutABodyIsABadRequest) {
    const Outcome o = parse("GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT_EQ(o.result, fatal(Status::BadRequest));
    EXPECT_EQ(o.heads, 0U);
}

TEST(RequestFraming, ContentLengthWithTransferEncodingIsABadRequestInEitherOrder) {
    for (const std::string_view request :
         {"PATCH /a HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n"
          "0\r\n\r\n",
          "PATCH /a HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n"
          "0\r\n\r\n"}) {
        const Outcome o = parse(request);
        EXPECT_EQ(o.result, fatal(Status::BadRequest)) << request;
        EXPECT_EQ(o.heads, 0U) << request;
    }
}

TEST(RequestFraming, ChunkedAsANonFinalCodingIsABadRequest) {
    const Outcome o = parse("POST /a HTTP/1.1\r\nTransfer-Encoding: chunked, gzip\r\n\r\n");
    EXPECT_EQ(o.result, fatal(Status::BadRequest));
    EXPECT_EQ(o.heads, 0U);
}

TEST(RequestFraming, SimilarlyNamedFieldIsNotATransferCoding) {
    RecordingSink sink;
    RequestParser parser{sink};
    const std::string_view input =
        "PATCH /b HTTP/1.1\r\nTransfer-Encoding-Hint: chunked\r\nContent-Length: 2\r\n\r\nok";

    EXPECT_EQ(parser.feed(bytes_of(input)), http::ParseProgress::MessageComplete);
    ASSERT_EQ(sink.requests().size(), 1U);
    EXPECT_EQ(sink.requests()[0].body, "ok");
}

} // namespace
